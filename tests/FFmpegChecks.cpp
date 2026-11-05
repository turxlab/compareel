#include "FFmpegReader.h"
#include "platform/Platform.h"
#include "VideoPlayer.h"
#include "VideoScanner.h"
#include "FileOperations.h"
#include <cstdio>
#include <stdexcept>
#ifndef _WIN32
#include <chrono>
#include <thread>
static void Sleep(unsigned milliseconds) { std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds)); }
#endif

static void require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
int wmain(int argc,wchar_t** argv) {
    Platform::ComInit com; Platform::MediaRuntime mf; int status=0;
    try {
        require((argc==4 || (argc==5 && std::wstring(argv[4])==L"--scrub")) && com.succeeded() && mf.succeeded(),"usage: FFmpegChecks local-file width height [--scrub]");
        const bool scrubOnly=argc==5;
        const std::wstring path=argv[1]; const auto original=FileOperations::ReadIdentity(path);
        const auto width=std::stoul(argv[2]),height=std::stoul(argv[3]);
        FFmpegReader direct; VideoFileInfo metadata;
        require(direct.Open(path) && direct.Metadata(metadata),"FFmpeg open/metadata");
        printf("metadata width=%u height=%u fps=%.4f duration=%.4f audio_channels=%d\n",metadata.width,metadata.height,metadata.fps,metadata.DurationSeconds(),metadata.audioChannels);
        require(metadata.width==width && metadata.height==height,"display dimensions/rotation");
        FFmpegReader::Sample frame;
        require(direct.VideoAt(13500000,frame) && frame.width==width && frame.height==height && frame.time>=13500000 && frame.time<15000000,"FFmpeg seek/converted pixels");
        const auto pts=frame.time; require(direct.VideoAt(pts,frame,true) && frame.time<pts,"FFmpeg previous frame");
        VideoFileInfo scanned; scanned.fullPath=path;
        std::atomic<bool> cancel{false};
        if(!scrubOnly) {
            require(VideoScanner::ExtractMetadataAndFrame(scanned,true,&cancel) && scanned.samples.size()==5,"actual scanner fallback/five samples");
            require(scanned.width==width && scanned.height==height && !scanned.corrupted && SUCCEEDED(scanned.error),"scanner dimensions/error");
            std::vector<FrameSignature> timeline;
            require(direct.SampleTimeline(5000000,24,timeline) && timeline.size()>=4,"FFmpeg SampleTimeline");
            const auto selfAlign=VideoScanner::EstimatePartialClipAlignment(scanned.fullPath,scanned.fullPath,&cancel,88.0);
            require((selfAlign.status==PartialClipStatus::PartialOverlap || selfAlign.status==PartialClipStatus::FullTimelineMatch) && std::abs(selfAlign.offset100ns)<=5000000,"partial clip self alignment");
            std::atomic<bool> preCancel{true};
            require(VideoScanner::EstimatePartialClipAlignment(scanned.fullPath,scanned.fullPath,&preCancel,88.0).status==PartialClipStatus::NotFound,"cancelled partial clip alignment");
        }
        VideoPlayer player; player.SetMuted(true); require(player.Load(path),"actual player load");
        auto wait=[&](const auto& condition) { for(int i=0;i<1500 && !condition() && SUCCEEDED(player.GetError());++i) Sleep(10); return condition(); };
        require(wait([&] { return player.IsLoaded(); }),"actual player decoder fallback");
        require(player.GetVideoWidth()==width && player.GetVideoHeight()==height,"actual player dimensions");
        if(scrubOnly) {
            for(int percent:{15,35,65,85}) {
                const auto start=std::chrono::steady_clock::now(); player.ScrubRatio(percent/100.0);
                require(wait([&] { return !player.IsSeeking(); }),"held-slider preview did not finish");
                printf("scrub target=%d%% displayed=%.3fs elapsed=%lldms\n",percent,player.GetPosition100ns()/1e7,
                    (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count());
            }
            std::set<int64_t> seen{player.GetPosition100ns()};
            for(int i=1;i<=30;++i) { player.ScrubRatio(0.1+0.7*i/30); Sleep(30); seen.insert(player.GetPosition100ns()); }
            require(seen.size()>=3,"no live previews while dragging");
            const auto target=metadata.duration100ns*83/100;
            const auto start=std::chrono::steady_clock::now(); player.SeekTo(target);
            require(wait([&] { return !player.IsSeeking(); }) && player.GetPosition100ns()>=target && player.GetPosition100ns()-target<1500000,"release latest target/exact PTS");
            printf("drag live_positions=%zu release=%lldms error=%.3fms\n",seen.size(),
                (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count(),(player.GetPosition100ns()-target)/1e4);
            // A newer request must replace an exact seek already in flight without a decoder error.
            player.SeekRatio(.79); Sleep(5); player.SeekRatio(.23);
            require(wait([&] { return !player.IsSeeking(); }) && std::abs(player.GetPosition100ns()-metadata.duration100ns*23/100)<1500000,"superseded exact seek");
        }
        player.SeekTo(13500000); require(wait([&] { return !player.IsSeeking(); }) && player.GetPosition100ns()>=13500000,"actual player exact seek");
        const auto selected=player.GetPosition100ns(); player.StepFrame(false);
        require(wait([&] { return !player.IsSeeking(); }) && player.GetPosition100ns()<selected,"actual player previous frame");
        player.StepFrame(true); require(wait([&] { return !player.IsSeeking(); }) && player.GetPosition100ns()==selected,"actual player next frame");
        player.Play(); const bool advanced=wait([&] { return player.GetPosition100ns()>selected; }); player.Pause();
        printf("playback advanced=%d position=%lld start=%lld HRESULT=0x%08lX\n",advanced,player.GetPosition100ns(),selected,static_cast<unsigned long>(player.GetError()));
        require(advanced && SUCCEEDED(player.GetError()),"actual player paced playback");
        player.SeekRatio(1); require(wait([&] { return !player.IsSeeking(); }) && SUCCEEDED(player.GetError()),"EOF seek");
        player.Close();
        cancel=true; FFmpegReader cancelled; require(!cancelled.Open(path,&cancel),"pre-cancelled open");
        require(FileOperations::ReadIdentity(path)==original,"source identity changed");
        printf("PASS local FFmpeg: metadata/rotation/converted BGRA, exact/previous/next/EOF seek, %s, actual muted playback, cancellation, source unchanged; no image output\n",scrubOnly?"live scrub/latest release":"five scanner samples");
    } catch(const std::exception& error) { fprintf(stderr,"FAIL %s\n",error.what()); status=1; }
    return status;
}
