#pragma once
#include "Common.h"
#include <memory>

class FFmpegReader {
public:
    struct Sample {
        bool video=false;
        int64_t time=0;
        uint32_t width=0,height=0;
        std::vector<uint8_t> bytes;
    };
    FFmpegReader();
    ~FFmpegReader();
    enum class DecodeMode { Configured, Cpu, PreferGpu };
    static void SetGpuEnabled(bool enabled);
    static bool GpuEnabled();
    bool Open(const std::wstring& path,std::atomic<bool>* cancel=nullptr,bool audio=false,DecodeMode mode=DecodeMode::Configured);
    bool HardwareDecoded() const;
    bool CpuFallbackUsed() const;
    bool Next(Sample& sample);
    bool Seek(int64_t time);
    bool VideoAt(int64_t target,Sample& sample,bool previous=false,bool exact=true,
                 const std::function<bool()>& superseded={});
    bool Metadata(VideoFileInfo& file) const;
    // orientations: how many normalized orientations each sample gets (8 = all, 1 = upright only, 0 = none).
    bool SampleTimeline(int64_t step100ns, size_t maxSamples, std::vector<FrameSignature>& outSamples,int orientations=8);
    bool HasAudio() const;
    bool Failed() const;
private:
    bool fallback(int64_t target);
    bool VideoAtImpl(int64_t target,Sample& sample,bool previous,bool exact,const std::function<bool()>& superseded);
    bool SampleTimelineImpl(int64_t step100ns,size_t maxSamples,std::vector<FrameSignature>& outSamples,int orientations);
    bool Next(Sample& sample,bool render);
    struct State;
    std::unique_ptr<State> state_;
};

bool ReadFFmpegSamples(VideoFileInfo& file,bool sampleVideo,std::atomic<bool>* cancel);
// Lists embedded subtitle tracks as "codec (language)" without decoding them.
bool ReadFFmpegSubtitleTracks(const std::wstring& path,std::vector<std::string>& tracks,std::atomic<bool>* cancel);
