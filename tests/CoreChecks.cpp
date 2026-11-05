#include "FileOperations.h"
#include "VideoScanner.h"
#include "VideoPlayer.h"
#include "ScanConcurrency.h"
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include "platform/Platform.h"
#ifdef _WIN32
#include "platform/WinApi.h"
#include <wrl/client.h>
#endif

static void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// Small portability helpers for the fixtures below.
#ifdef _WIN32
static void SetReadOnly(const std::filesystem::path& path, bool readOnly) { SetFileAttributesW(path.c_str(), readOnly ? FILE_ATTRIBUTE_READONLY : FILE_ATTRIBUTE_NORMAL); }
static bool IsReadOnly(const std::filesystem::path& path) { return (GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_READONLY) != 0; }
static bool ReadOnlyIsEnforced() { return true; }
// An open handle without delete sharing blocks recycling and moving on Windows; POSIX has no such lock.
struct OpenLock {
    HANDLE handle;
    explicit OpenLock(const std::filesystem::path& path) : handle(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr)) {}
    ~OpenLock() { release(); }
    bool valid() const { return handle != INVALID_HANDLE_VALUE; }
    void release() { if (valid()) CloseHandle(handle); handle = INVALID_HANDLE_VALUE; }
};
constexpr bool kOpenFilesBlockRemoval = true;
#else
#include <unistd.h>
static void SetReadOnly(const std::filesystem::path& path, bool readOnly) {
    std::filesystem::permissions(path, std::filesystem::perms::owner_write | std::filesystem::perms::group_write | std::filesystem::perms::others_write,
                                 readOnly ? std::filesystem::perm_options::remove : std::filesystem::perm_options::add);
}
static bool IsReadOnly(const std::filesystem::path& path) { return (std::filesystem::status(path).permissions() & std::filesystem::perms::owner_write) == std::filesystem::perms::none; }
static bool ReadOnlyIsEnforced() { return geteuid() != 0; } // root may write to any file
struct OpenLock {
    explicit OpenLock(const std::filesystem::path&) {}
    bool valid() const { return true; }
    void release() {}
};
constexpr bool kOpenFilesBlockRemoval = false;
static void Sleep(unsigned milliseconds) { std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds)); }
#endif

static void FileOperationChecks(const std::filesystem::path& directory) {
    auto file = directory / L"recycle-test.txt";
    { std::ofstream out(file); out << "Compareel disposable file operation test"; }
    auto identity = FileOperations::ReadIdentity(file.wstring());
    Check(identity.valid, "identity was not read");
    auto wrong = identity;
    ++wrong.index;
    Check(!FileOperations::Remove(nullptr, file.wstring(), true, wrong).succeeded(), "changed target accepted");
    Check(std::filesystem::exists(file), "changed-target check removed the file");
    SetReadOnly(file, true);
    if (ReadOnlyIsEnforced()) Check(!FileOperations::Remove(nullptr, file.wstring(), true, identity).succeeded(), "read-only target accepted");
    Check(IsReadOnly(file), "read-only attribute was changed");
    SetReadOnly(file, false);
    if (kOpenFilesBlockRemoval) {
        OpenLock lock(file);
        Check(lock.valid(), "test file lock failed");
        auto failed = FileOperations::Remove(nullptr, file.wstring(), true, identity);
        lock.release();
        Check(!failed.succeeded() && std::filesystem::exists(file), "failed recycle removed a locked file");
    }
    auto result = FileOperations::Remove(nullptr, file.wstring(), true, identity);
    if (!result.succeeded()) fwprintf(stderr, L"Recycle error: %ls\n", result.message().c_str());
    Check(result.succeeded() && !std::filesystem::exists(file), "real recycle did not complete");
    Check(!FileOperations::Remove(nullptr, file.wstring(), true).succeeded(), "missing target was reported as deleted");
    puts("PASS file operations: identity, read-only, locked recycle failure, actual recycle, missing target");
}

static void TransferChecks(const std::filesystem::path& directory) {
    using namespace FileOperations;
    const auto source=directory/L"source.txt",destination=directory/L"copy.txt",moved=directory/L"moved.txt";
    { std::ofstream file(source,std::ios::binary); file<<std::string(8*1024*1024,'x'); }
    const auto identity=ReadIdentity(source.wstring()); Check(identity.valid,"transfer source identity");
    std::atomic<bool> cancel{false};
    auto wrong=identity; ++wrong.index;
    Check(!Transfer(source.wstring(),destination.wstring(),TransferKind::Copy,wrong,cancel).succeeded() && !std::filesystem::exists(destination),"changed source copied");
    cancel=true; Check(!Transfer(source.wstring(),destination.wstring(),TransferKind::Copy,identity,cancel).succeeded() && !std::filesystem::exists(destination),"pre-cancelled transfer wrote target"); cancel=false;
    auto copied=Transfer(source.wstring(),destination.wstring(),TransferKind::Copy,identity,cancel);
    Check(copied.succeeded() && copied.copied && !copied.moved && copied.destinationIdentity.valid && copied.contentHash.size()==64 && ReadIdentity(source.wstring())==identity,"actual verified copy");
    auto collision=Transfer(source.wstring(),destination.wstring(),TransferKind::Move,identity,cancel);
    Check(!collision.succeeded() && ReadIdentity(source.wstring())==identity && ReadIdentity(destination.wstring())==copied.destinationIdentity,"target collision overwrote a file");
    auto badDestination=copied.destinationIdentity; ++badDestination.index;
    Check(!UndoTransfer(source.wstring(),destination.wstring(),TransferKind::Copy,identity,badDestination,copied.contentHash,cancel).succeeded() && std::filesystem::exists(destination),"undo accepted changed destination");
    const auto unavailable=directory/L"source.unavailable"; std::filesystem::rename(source,unavailable);
    Check(!UndoTransfer(source.wstring(),destination.wstring(),TransferKind::Copy,identity,copied.destinationIdentity,copied.contentHash,cancel).succeeded() && std::filesystem::exists(destination),"undo deleted the only remaining copy");
    std::filesystem::rename(unavailable,source);
    Check(UndoTransfer(source.wstring(),destination.wstring(),TransferKind::Copy,identity,copied.destinationIdentity,copied.contentHash,cancel).succeeded() && !std::filesystem::exists(destination) && ReadIdentity(source.wstring())==identity,"actual copy undo");
    if (kOpenFilesBlockRemoval) {
        OpenLock lock(source);
        Check(lock.valid(),"move failure fixture lock");
        // Same volume: a move is one rename, so a locked source fails before anything is written.
        auto partial=Transfer(source.wstring(),destination.wstring(),TransferKind::Move,identity,cancel); lock.release();
        Check(!partial.succeeded() && !partial.copied && !partial.moved && ReadIdentity(source.wstring())==identity && !std::filesystem::exists(destination),"locked same-volume move wrote a target");
    }
    // The cross-volume path copies, verifies and only then recycles the source.
    sameVolumeRename=false;
    if (kOpenFilesBlockRemoval) {
        OpenLock lock(source);
        auto copiedPartial=Transfer(source.wstring(),destination.wstring(),TransferKind::Move,identity,cancel); lock.release();
        Check(!copiedPartial.succeeded() && copiedPartial.copied && !copiedPartial.moved && ReadIdentity(source.wstring())==identity && std::filesystem::exists(destination),"partial move did not retain source and verified copy");
        Check(UndoTransfer(source.wstring(),destination.wstring(),TransferKind::Copy,identity,copiedPartial.destinationIdentity,copiedPartial.contentHash,cancel).succeeded() && !std::filesystem::exists(destination),"partial move must undo its completed copy phase");
    }
    auto interrupted=Transfer(source.wstring(),destination.wstring(),TransferKind::Move,identity,cancel,[&](uint64_t done,uint64_t) { if(done) cancel=true; });
    Check(!interrupted.succeeded() && !std::filesystem::exists(destination) && ReadIdentity(source.wstring())==identity,"in-copy cancellation did not preserve source/clean target"); cancel=false;
    sameVolumeRename=true;
    auto callbackFailure=Transfer(source.wstring(),destination.wstring(),TransferKind::Copy,identity,cancel,[](uint64_t,uint64_t) { throw std::runtime_error("test progress callback"); });
    Check(!callbackFailure.succeeded() && !std::filesystem::exists(destination) && ReadIdentity(source.wstring())==identity,"callback failure escaped or left target");
    SetReadOnly(source,true); Check(IsReadOnly(source),"readonly fixture");
    if (ReadOnlyIsEnforced()) Check(!Transfer(source.wstring(),moved.wstring(),TransferKind::Move,identity,cancel).succeeded() && !std::filesystem::exists(moved),"readonly source move created target");
    SetReadOnly(source,false);
    auto transferred=Transfer(source.wstring(),moved.wstring(),TransferKind::Move,identity,cancel);
    Check(transferred.succeeded() && transferred.copied && transferred.moved && !std::filesystem::exists(source) && transferred.destinationIdentity==identity,"same-volume move must rename the verified source");
    { std::ofstream occupied(source); occupied<<"do not overwrite"; }
    Check(!UndoTransfer(source.wstring(),moved.wstring(),TransferKind::Move,identity,transferred.destinationIdentity,transferred.contentHash,cancel).succeeded() && std::filesystem::file_size(source)==16,"move undo overwrote occupied original path");
    std::filesystem::remove(source);
    Check(!UndoTransfer(source.wstring(),moved.wstring(),TransferKind::Move,identity,transferred.destinationIdentity,std::string(64,'0'),cancel).succeeded() && !std::filesystem::exists(source),"move undo accepted wrong hash");
    auto undone=UndoTransfer(source.wstring(),moved.wstring(),TransferKind::Move,identity,transferred.destinationIdentity,transferred.contentHash,cancel);
    Check(undone.succeeded() && undone.moved && !std::filesystem::exists(moved) && std::filesystem::exists(source),"actual move undo");
    HRESULT hashError=S_OK; Check(ContentHash(source.wstring(),cancel,hashError)==transferred.contentHash && SUCCEEDED(hashError),"move undo restored different bytes");
    std::filesystem::remove(source);
    puts("PASS transfers: verified copy/move and real undo, no overwrite/changed identity/wrong hash/readonly guards, in-copy cancellation, same-volume rename, occupied original path; only disposable files, one volume");
}

static void WriteBitmap(const std::filesystem::path& path, uint32_t width, uint32_t height,
                        const std::vector<uint8_t>& pixels) {
    // 32-bit top-down BMP: 14-byte file header, 40-byte info header, little-endian fields.
    std::vector<uint8_t> header(54, 0);
    auto put = [&](size_t at, uint32_t value, int bytes) { for (int i = 0; i < bytes; ++i) header[at + i] = static_cast<uint8_t>(value >> (8 * i)); };
    put(0, 0x4D42, 2); put(2, static_cast<uint32_t>(54 + pixels.size()), 4); put(10, 54, 4);
    put(14, 40, 4); put(18, width, 4); put(22, static_cast<uint32_t>(-static_cast<int32_t>(height)), 4);
    put(26, 1, 2); put(28, 32, 2);
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
    output.write(reinterpret_cast<const char*>(pixels.data()), pixels.size());
}

static void MediaChecks(const std::wstring& path,uint32_t expectedWidth=1080,uint32_t expectedHeight=1920,bool artifacts=true) {
    constexpr int w = 16, h = 16, stride = w * 4;
    std::vector<uint8_t> positive(w * h * 4), negative(w * h * 4);
    for (int y = 0; y < h; ++y) for (int x = 0; x < stride; ++x) {
        uint8_t value = static_cast<uint8_t>((y * 17 + x * 3) % 256);
        positive[y * stride + x] = value;
        negative[(h - 1 - y) * stride + x] = value;
    }
    auto a = PerceptualHash::ComputeSignatureFromBGRA(positive.data(), w, h, stride);
    auto b = PerceptualHash::ComputeSignatureFromBGRA(negative.data() + (h - 1) * stride, w, h, -stride);
    Check(PerceptualHash::CompareSignatures(a, b) == 100, "negative stride changed the image signature");
    std::vector<uint8_t> thumbA, thumbB;
    PerceptualHash::GenerateThumbnailBGRA(positive.data(), w, h, stride, thumbA);
    PerceptualHash::GenerateThumbnailBGRA(negative.data() + (h - 1) * stride, w, h, -stride, thumbB);
    Check(thumbA == thumbB, "negative stride changed the thumbnail");

    VideoFileInfo info;
    info.fullPath = path;
    Check(VideoScanner::ExtractMetadataAndFrame(info), "scanner frame decode failed");
    Check(info.width == expectedWidth && info.height == expectedHeight, "visible dimensions include decoder padding");
    if(artifacts) {
        std::filesystem::create_directories(".build/check-artifacts");
        WriteBitmap(L".build/check-artifacts/scanner-thumbnail.bmp", THUMB_W, THUMB_H, info.thumbBGRA);
    }
    VideoPlayer player;
    player.SetMuted(true);
    Check(player.Load(path), "player load was rejected");
    for (int i = 0; i < 300 && !player.IsLoaded() && SUCCEEDED(player.GetError()); ++i) Sleep(10);
    Check(player.IsLoaded(), "player frame decode failed");
    auto frame = player.GetFrame();
    Check(frame && frame->width == expectedWidth && frame->height == expectedHeight, "player retained padded or invalid frame");
    if(artifacts) WriteBitmap(L".build/check-artifacts/player-frame.bmp", frame->width, frame->height, frame->pixels);
    player.SeekTo(13500000);
    for (int i = 0; i < 300 && player.IsSeeking(); ++i) Sleep(10);
    double position = player.GetPosition100ns() / 10000000.0;
    printf("Seek request 1.350s -> %.3fs\n", position);
    Check(!player.IsSeeking() && position >= 1.35 && position < 1.5, "seek did not reach the target frame");
    int64_t selected=player.GetPosition100ns();
    player.StepFrame(false); for(int i=0;i<300 && player.IsSeeking();++i) Sleep(10);
    Check(player.GetPosition100ns()<selected,"previous frame stayed on the same rounded timestamp");
    player.StepFrame(true); for(int i=0;i<300 && player.IsSeeking();++i) Sleep(10);
    Check(player.GetPosition100ns()==selected,"next frame skipped a presented frame");
    player.Close();
    puts("PASS media: signed stride, dynamic media type, visible crop, scanner/player pixels, target seek");
}

static void MatchingChecks(const std::filesystem::path& directory) {
    for (const auto& [name, bytes] : std::vector<std::pair<std::wstring, std::string>>{
        {L"a.mp4", "abc"}, {L"b.mp4", "abc"}, {L"c.mp4", "abd"}}) {
        std::ofstream file(directory / name, std::ios::binary);
        file << bytes;
    }
    std::atomic<bool> cancel{false};
    HRESULT error = S_OK;
    Check(FileOperations::ContentHash((directory / L"a.mp4").wstring(), cancel, error) ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA256 known answer failed");
    std::error_code linkError; std::filesystem::create_hard_link(directory / L"a.mp4", directory / L"alias.mp4", linkError);
    Check(!linkError, "hardlink fixture failed");
    ScanCriteria criteria;
    criteria.matchContent = true;
    criteria.loadPreviews = false;
    criteria.protectedFolders = {directory.wstring()};
    auto files = VideoScanner::DiscoverVideos({directory.wstring()}, false, cancel, nullptr, &criteria);
    Check(files.size() == 3, "physical alias counted twice");
    Check(files[0].protectedFile, "reference protection was not recorded");
    std::vector<VideoGroup> groups;
    VideoScanner::AnalyzeAndGroup(files, criteria, groups, cancel);
    Check(groups.size() == 1 && groups[0].fileIndices.size() == 2, "same size was confused with identical contents");
    // Duration tolerance gives a non-transitive chain: 100~110 and 110~120, but 100 and 120 differ by more than 10.
    criteria.matchContent = false; criteria.matchSize = false; criteria.matchDuration = true; criteria.durationTol100ns = 10;
    std::vector<VideoFileInfo> chain(3);
    for (int i = 0; i < 3; ++i) { chain[i].fileSize = 100; chain[i].duration100ns = 100 + i * 10; chain[i].metadataLoaded = true; chain[i].frameLoaded = true; }
    VideoScanner::AnalyzeAndGroup(chain, criteria, groups, cancel);
    Check(groups.size() == 1 && groups[0].fileIndices.size() == 2, "indirect match entered a direct-reference group");
    const auto& group = groups[0];
    Check(VideoScanner::FilesMatch(chain[group.fileIndices[0]], chain[group.fileIndices[1]], criteria), "group reference does not match candidate");
    chain[1].protectedFile=true;
    VideoScanner::AnalyzeAndGroup(chain, criteria, groups, cancel);
    Check(groups.size()==1 && groups[0].fileIndices.size()==3 && groups[0].fileIndices[0]==1,"protected reference was not preferred");
    chain[1].deleted=true;
    VideoScanner::AnalyzeAndGroup(chain, criteria, groups, cancel);
    Check(groups.empty(),"removing a bridge reference left unrelated files grouped");
    for (const auto& name : {L"a.mp4", L"b.mp4", L"c.mp4", L"alias.mp4"}) std::filesystem::remove(directory / name);
    puts("PASS matching: SHA256 known answer, actual files, physical alias, protected folder, direct-reference grouping");
}

static void DurationPrefilterChecks() {
    ScanCriteria criteria; criteria.matchSize=false; criteria.matchDuration=true; criteria.matchVisualSamples=true;
    criteria.durationTol100ns=10; criteria.loadPreviews=false;
    std::atomic<bool> cancel{false}; std::vector<VideoGroup> groups; ScanMetrics metrics;
    auto analyze=[&](std::initializer_list<int64_t> durations) {
        std::vector<VideoFileInfo> files;
        for(auto duration:durations) {
            VideoFileInfo file; file.fileSize=100; file.metadataLoaded=true; file.frameLoaded=true;
            file.duration100ns=duration; file.sigFirst.valid=true;
            file.samples.resize(5); for(auto& sample:file.samples) sample.valid=true;
            files.push_back(file);
        }
        VideoScanner::AnalyzeAndGroup(files,criteria,groups,cancel,nullptr,nullptr,&metrics);
        return files;
    };
    auto files=analyze({100,110,121,500,500,0,-1});
    Check(groups.size()==2 && metrics.sampleCandidates==4 && metrics.durationRejected==3,"duration boundary/unique/invalid filtering failed");
    for(const auto& group:groups) for(int index:group.fileIndices)
        Check(VideoScanner::FilesMatch(files[group.fileIndices[0]],files[index],criteria),"duration filter produced an invalid group");
    analyze({100,110,120});
    Check(metrics.sampleCandidates==3 && groups.size()==1 && groups[0].fileIndices.size()==2,"duration chain changed direct grouping");
    criteria.durationTol100ns=0;
    analyze({100,100,101});
    Check(metrics.sampleCandidates==2 && groups.size()==1,"equal duration with zero tolerance failed");
    analyze({100,200}); Check(groups.empty() && metrics.sampleCandidates==0,"all unique durations were sampled");
    criteria.matchDuration=false;
    analyze({0,100,200}); Check(metrics.sampleCandidates==3 && groups.size()==1,"duration filter applied when disabled");
    criteria.matchDuration=true;
    files=analyze({100,100}); files[1].corrupted=true; files[1].error=E_FAIL;
    VideoScanner::AnalyzeAndGroup(files,criteria,groups,cancel,nullptr,nullptr,&metrics);
    Check(groups.empty() && metrics.sampleCandidates==0,"bad metadata supplied a duration neighbor");
    cancel.store(true); analyze({100,100}); Check(groups.empty(),"cancelled duration scan returned results");
    puts("PASS duration filter: tolerance boundary, equal/unique/invalid durations, direct grouping, disabled criterion, corrupt metadata, cancellation");
}

static void VisualChecks(wchar_t** paths) {
    std::vector<VideoFileInfo> files(4);
    for(int i=0;i<4;++i) {
        files[i].fullPath=paths[i];
        Check(VideoScanner::ExtractMetadataAndFrame(files[i],true),"visual fixture could not be decoded");
        Check(files[i].samples.size()==5,"five positions were not sampled");
    }
    ScanCriteria criteria; criteria.matchSize=false; criteria.matchDuration=true; criteria.matchVisualSamples=true;
    double similarity=0;
    printf("Durations: %lld %lld; corrupt: %d %d\n",files[0].duration100ns,files[1].duration100ns,files[0].corrupted,files[1].corrupted);
    for(int i=0;i<5;++i) printf("Sample %d valid=%d/%d similarity=%.2f\n",i,files[0].samples[i].valid,files[1].samples[i].valid,PerceptualHash::CompareSignatures(files[0].samples[i],files[1].samples[i]));
    Check(VideoScanner::FilesMatch(files[0],files[1],criteria,&similarity),"re-encoded resized video was not recognized");
    printf("Resized encode visual similarity: %.2f\n",similarity);
    Check(!VideoScanner::FilesMatch(files[0],files[2],criteria),"common intro was confused with the whole video");
    Check(!VideoScanner::FilesMatch(files[3],files[3],criteria),"blank video was treated as trustworthy visual evidence");
    auto shortClip=files[0]; shortClip.samples.resize(2);
    Check(!VideoScanner::FilesMatch(files[0],shortClip,criteria),"insufficient temporal evidence was accepted");
    puts("PASS visual samples: resized encode, common intro rejected, blank evidence rejected, missing samples rejected");
}

static void ShortClipChecks(const std::wstring& path) {
    VideoFileInfo file; file.fullPath=path;
    Check(VideoScanner::ExtractMetadataAndFrame(file,true),"short clip could not be decoded");
    unsigned valid=0;
    std::vector<int64_t> timestamps;
    for(const auto& sample:file.samples) if(sample.valid) {
        Check(std::find(timestamps.begin(),timestamps.end(),sample.timestamp100ns)==timestamps.end(),"same presented frame counted twice");
        timestamps.push_back(sample.timestamp100ns); ++valid;
    }
    ScanCriteria criteria; criteria.matchSize=false; criteria.matchVisualSamples=true;
    printf("Short clip: %.3fs, %u distinct valid samples\n",file.DurationSeconds(),valid);
    Check(valid<3 && !VideoScanner::FilesMatch(file,file,criteria),"short clip accepted without enough distinct frames");
    puts("PASS short clip: repeated timestamps do not multiply visual evidence");
}

static void AudioChecks(const std::wstring& path) {
    VideoPlayer player; player.Load(path);
    for(int i=0;i<300 && !player.IsLoaded() && SUCCEEDED(player.GetError());++i) Sleep(10);
    Check(player.IsLoaded(),"audio fixture failed to open");
    player.SeekTo(13500000);
    for(int i=0;i<300 && player.IsSeeking();++i) Sleep(10);
    player.PlayAt(std::chrono::steady_clock::now()+std::chrono::milliseconds(80),13500000);
    for(int i=0;i<300 && player.GetLastSubmittedAudioTime100ns()<0 && player.GetAudioError()==Platform::kAudioErrorNone;++i) Sleep(5);
    auto audio=player.GetLastSubmittedAudioTime100ns();
    printf("First PCM after seek: %.5fs, waveOut status=%u\n",audio/10000000.0,player.GetAudioError());
    Check(player.GetAudioError()==Platform::kAudioErrorNone && audio>=13500000 && audio<15000000,"PCM preroll was not trimmed or audio output failed");
    Sleep(200);
    Check(player.GetLastSubmittedAudioTime100ns()-player.GetPosition100ns()<2000000,"PCM queue ran ahead of the shared clock");
    player.SetMuted(true); Sleep(50); auto muted=player.GetLastSubmittedAudioTime100ns(); Sleep(100);
    Check(player.GetLastSubmittedAudioTime100ns()==muted,"PCM submitted while muted");
    player.Pause(); player.Close();
    puts("PASS audio output: real waveOut submission, seek preroll, clock pacing, mute");
}

static void ScrubChecks(const std::wstring& path) {
    VideoPlayer player;
    player.SetMuted(true);
    Check(player.Load(path), "scrub fixture failed to open");
    for (int i = 0; i < 500 && !player.IsLoaded() && SUCCEEDED(player.GetError()); ++i) Sleep(10);
    Check(player.IsLoaded(), "scrub fixture failed to decode initial frame");
    int64_t dur = player.GetDuration100ns();
    Check(dur > 5000000, "scrub fixture duration too short");

    std::set<int64_t> seenPositions;
    seenPositions.insert(player.GetPosition100ns());
    auto dragStart = std::chrono::steady_clock::now();
    for (int step = 1; step <= 20; ++step) {
        double ratio = 0.08 + 0.78 * (double(step) / 20.0);
        player.ScrubRatio(ratio);
        Sleep(12);
        seenPositions.insert(player.GetPosition100ns());
    }
    auto settleStart = std::chrono::steady_clock::now();
    for (int i = 0; i < 300 && player.IsSeeking(); ++i) {
        Sleep(5);
        seenPositions.insert(player.GetPosition100ns());
    }
    auto settleMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - settleStart).count();
    auto totalDragMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - dragStart).count();
    printf("Scrub 20 drag steps (dur=%.1fs): %zu distinct positions shown during drag, post-drag settle=%lldms, total=%lldms\n",
           dur / 10000000.0, seenPositions.size(), static_cast<long long>(settleMs), static_cast<long long>(totalDragMs));
    Check(!player.IsSeeking(), "scrub did not finish");
    Check(seenPositions.size() >= 3, "live slider scrubbing did not update displayed frames during drag");

    double lastRatio = 0.08 + 0.78;
    int64_t dragEndTarget = static_cast<int64_t>(dur * lastRatio);
    auto dragReleaseStart = std::chrono::steady_clock::now();
    player.SeekTo(dragEndTarget);
    for (int i = 0; i < 600 && player.IsSeeking(); ++i) Sleep(5);
    auto dragReleaseMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - dragReleaseStart).count();
    int64_t dragEndPos = player.GetPosition100ns();
    printf("Drag-end release target=%.3fs -> reached=%.3fs in %lldms\n",
           dragEndTarget / 10000000.0, dragEndPos / 10000000.0, static_cast<long long>(dragReleaseMs));
    Check(!player.IsSeeking() && dragEndPos >= dragEndTarget, "drag-end release did not reach target PTS");

    int64_t exactTarget = std::min<int64_t>(dur - 2000000, std::max<int64_t>(13500000, static_cast<int64_t>(dur * 0.37)));
    auto releaseStart = std::chrono::steady_clock::now();
    player.SeekTo(exactTarget);
    for (int i = 0; i < 600 && player.IsSeeking(); ++i) Sleep(5);
    auto releaseMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - releaseStart).count();
    int64_t finalPos = player.GetPosition100ns();
    printf("Jump exact seek target=%.3fs -> reached=%.3fs in %lldms\n",
           exactTarget / 10000000.0, finalPos / 10000000.0, static_cast<long long>(releaseMs));
    Check(!player.IsSeeking() && finalPos >= exactTarget, "release exact seek did not reach target PTS");

    auto redundantStart = std::chrono::steady_clock::now();
    player.SeekTo(exactTarget);
    for (int i = 0; i < 100 && player.IsSeeking(); ++i) Sleep(2);
    auto redundantMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - redundantStart).count();
    Check(!player.IsSeeking() && redundantMs < 15, "redundant exact seek was not coalesced");

    // Verify backward intra-GOP scrub does not flash an earlier keyframe far before backwardTarget
    int64_t backwardTarget = std::max<int64_t>(0, finalPos - 1000000); // 100ms earlier
    int64_t minSeenDuringBackScrub = finalPos;
    player.ScrubTo(backwardTarget);
    for (int i = 0; i < 600 && player.IsSeeking(); ++i) {
        minSeenDuringBackScrub = std::min(minSeenDuringBackScrub, player.GetPosition100ns());
        Sleep(2);
    }
    minSeenDuringBackScrub = std::min(minSeenDuringBackScrub, player.GetPosition100ns());
    printf("Backward intra-GOP scrub from %.3fs to %.3fs: minDisplayed=%.3fs, settled=%.3fs\n",
           finalPos / 10000000.0, backwardTarget / 10000000.0,
           minSeenDuringBackScrub / 10000000.0, player.GetPosition100ns() / 10000000.0);
    Check(!player.IsSeeking() && SUCCEEDED(player.GetError()), "backward intra-GOP scrub failed");
    Check(std::abs(backwardTarget - minSeenDuringBackScrub) <= std::abs(backwardTarget - finalPos) + 500000,
          "backward intra-GOP scrub flashed a keyframe further from target than the starting frame");

    // Verify scrubbing all the way to 100% (EOF) and releasing at 100% does not fail with E_FAIL
    player.ScrubRatio(1.0);
    player.SeekRatio(1.0);
    for (int i = 0; i < 600 && player.IsSeeking(); ++i) Sleep(5);
    int64_t eofPos = player.GetPosition100ns();
    printf("EOF 100%% scrub+release (dur=%.3fs) -> reached=%.3fs, hr=0x%08lX\n",
           dur / 10000000.0, eofPos / 10000000.0, static_cast<unsigned long>(player.GetError()));
    Check(!player.IsSeeking() && SUCCEEDED(player.GetError()), "100% EOF scrub/release set player error");
    Check(eofPos >= std::max<int64_t>(0, dur - 20000000), "100% EOF scrub/release did not preserve last frame near EOF");

    player.Close();
    puts("PASS scrub: coalesced live preview during drag, backward intra-GOP guard, 100% EOF safety, exact PTS on release");
}

static void SamplingResumeChecks(const std::wstring& path) {
    VideoFileInfo file; file.fullPath=path;
    Check(VideoScanner::ExtractMetadataOnly(file),"resume fixture metadata");
    std::atomic<bool> cancel{true};
    Check(!VideoScanner::ExtractMetadataAndFrame(file,true,&cancel) && !file.frameLoaded && file.samples.empty(),"cancelled read changed completion state");
    // Model an interrupted object from an older build; it must not take the cached fast path.
    file.frameLoaded=true; file.sigFirst.valid=true; file.samples.resize(2);
    cancel=false;
    Check(VideoScanner::ExtractMetadataAndFrame(file,true,&cancel),"resume decode failed");
    Check(file.frameLoaded && file.samples.size()==5,"partial sample set was treated as complete");
    unsigned valid=0; for(const auto& sample:file.samples) valid+=sample.valid;
    Check(valid>=3,"resumed read did not produce usable evidence");
    puts("PASS sampling resume: pre-cancel unchanged, partial evidence redecoded at all five positions");
}
#ifdef _WIN32
// Media Foundation behaviour checks (Windows only).
static void UnsupportedCodecChecks(const std::wstring& path) {
    using Microsoft::WRL::ComPtr;
    const auto identity=FileOperations::ReadIdentity(path); VideoFileInfo file; file.fullPath=path;
    Check(identity.valid && VideoScanner::ExtractMetadataOnly(file),"unsupported codec metadata unavailable");
    Check(VideoScanner::ExtractMetadataAndFrame(file,true) && SUCCEEDED(file.error) && file.samples.size()==5,"local decoder fallback failed");
    ComPtr<IMFAttributes> attrs; ComPtr<IMFSourceReader> legacy; ComPtr<IMFMediaType> type;
    Check(SUCCEEDED(MFCreateAttributes(&attrs,2)),"legacy attributes");
    attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING,TRUE); attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS,TRUE);
    Check(SUCCEEDED(MFCreateSourceReaderFromURL(path.c_str(),attrs.Get(),&legacy)) && SUCCEEDED(MFCreateMediaType(&type)),"legacy reader");
    type->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video); type->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_RGB32);
    Check(legacy->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM),nullptr,type.Get())==MF_E_TOPO_CODEC_NOT_FOUND,"failure differs from original reader");
    Check(FileOperations::ReadIdentity(path)==identity,"source changed during codec check");
    puts("PASS codec fallback: Windows decoder unavailable, local FFmpeg produced five samples, source unchanged");
}
static void TrackProbe(const std::wstring& path) {
    Microsoft::WRL::ComPtr<IMFSourceReader> reader;
    Check(SUCCEEDED(MFCreateSourceReaderFromURL(path.c_str(),nullptr,&reader)),"track probe open");
    unsigned subtitles=0;
    for(DWORD stream=0;stream<64;++stream) {
        Microsoft::WRL::ComPtr<IMFMediaType> type;
        HRESULT error=reader->GetNativeMediaType(stream,0,&type);
        if(error==MF_E_INVALIDSTREAMNUMBER) break;
        Check(SUCCEEDED(error),"track probe native type");
        GUID major{},subtype{}; type->GetGUID(MF_MT_MAJOR_TYPE,&major); type->GetGUID(MF_MT_SUBTYPE,&subtype);
        const char* kind=major==MFMediaType_Video?"video":major==MFMediaType_Audio?"audio":major==MFMediaType_Subtitle?"subtitle":major==MFMediaType_SAMI?"SAMI":"other";
        subtitles+=major==MFMediaType_Subtitle || major==MFMediaType_SAMI;
        printf("PROBE stream=%lu kind=%s subtype_Data1=0x%08lX\n",stream,kind,subtype.Data1);
    }
    printf("PROBE reader-reported subtitle tracks=%u; absence here is not proof of no embedded subtitles\n",subtitles);
    Microsoft::WRL::ComPtr<IMFSourceResolver> resolver; Microsoft::WRL::ComPtr<IUnknown> object;
    Microsoft::WRL::ComPtr<IMFMediaSource> source; MF_OBJECT_TYPE kind=MF_OBJECT_INVALID;
    Check(SUCCEEDED(MFCreateSourceResolver(&resolver)) && SUCCEEDED(resolver->CreateObjectFromURL(path.c_str(),MF_RESOLUTION_MEDIASOURCE,nullptr,&kind,&object)) && SUCCEEDED(object.As(&source)),"media source probe open");
    Microsoft::WRL::ComPtr<IMFPresentationDescriptor> presentation;
    HRESULT result=source->CreatePresentationDescriptor(&presentation); DWORD count=0;
    if(SUCCEEDED(result)) result=presentation->GetStreamDescriptorCount(&count);
    if(SUCCEEDED(result)) for(DWORD i=0;i<count;++i) {
        BOOL selected=FALSE; Microsoft::WRL::ComPtr<IMFStreamDescriptor> descriptor; Microsoft::WRL::ComPtr<IMFMediaTypeHandler> handler; GUID major{};
        result=presentation->GetStreamDescriptorByIndex(i,&selected,&descriptor);
        if(SUCCEEDED(result)) result=descriptor->GetMediaTypeHandler(&handler);
        if(SUCCEEDED(result)) result=handler->GetMajorType(&major);
        if(FAILED(result)) break;
        printf("PROBE media-source stream=%lu subtitle=%d other=%d\n",i,major==MFMediaType_Subtitle || major==MFMediaType_SAMI,major!=MFMediaType_Video && major!=MFMediaType_Audio);
    }
    source->Shutdown(); Check(SUCCEEDED(result),"presentation track probe");
}
#endif

static void ResourceBudgetChecks() {
    constexpr uint64_t GiB=1024ULL*1024*1024;
    auto limit=[](unsigned request,size_t files,unsigned cpu,uint64_t bytes,uint64_t pixels=0,bool decode=true) {
        return LimitScanConcurrency(request,files,cpu,bytes,pixels,decode).workers;
    };
    Check(limit(16,100,8,64*GiB)==4,"video CPU headroom");
    Check(limit(16,100,32,2*GiB)==2,"low-memory decoder limit");
    Check(limit(16,100,32,GiB)==1,"memory pressure serial fallback");
    Check(limit(2,100,32,64*GiB)==2,"user limit increased");
    Check(limit(16,1,32,64*GiB)==1 && limit(16,0,32,64*GiB)==0,"candidate bound");
    Check(limit(0,100,0,0)==1,"unknown resources fallback");
    Check(limit(16,100,32,64*GiB,UINT64_MAX)==1,"dimension overflow");
    Check(limit(16,100,8,64*GiB,0,false)==8,"metadata CPU limit");
    Check(limit(16,100,32,4*GiB,7680ULL*4320)==1,"8K decoder budget");
    Check(VolumeConcurrencyCap(StorageTier::Rotational,true)==1 && VolumeConcurrencyCap(StorageTier::Rotational,false)==2,"HDD per-volume cap");
    Check(VolumeConcurrencyCap(StorageTier::NetworkOrRemovable,true)==2 && VolumeConcurrencyCap(StorageTier::Unknown,true)==2,"network/unknown conservative cap");
    Check(VolumeConcurrencyCap(StorageTier::SolidState,true)==8 && VolumeConcurrencyCap(StorageTier::SolidState,false)==16,"SSD per-volume cap");
#ifdef _WIN32
    Check(DetectStorageTier(L"\\\\nas\\share\\clip.mp4")==StorageTier::NetworkOrRemovable,"UNC network tier detection");
#endif
    Check(DetectStorageTier(L"")==StorageTier::Unknown,"empty path tier fallback");
    const auto localTier=DetectStorageTier(L"C:\\");
    Check(StorageTierKey(static_cast<int>(localTier))[0]!='\0',"local storage tier label");
    Check(LimitScanConcurrency(16,100,16,64*GiB,0,true,StorageTier::Rotational,1).workers==1,"single HDD decode concurrency cap");
    Check(LimitScanConcurrency(16,100,16,64*GiB,0,false,StorageTier::Rotational,1).workers==2,"single HDD metadata concurrency cap");
    Check(LimitScanConcurrency(16,100,16,64*GiB,0,true,StorageTier::Rotational,3).workers==3,"multi-HDD volume concurrency cap");
    Check(LimitScanConcurrency(16,100,16,64*GiB,0,true,StorageTier::Unknown,1).workers==2,"unknown disk conservative concurrency cap");
    {
        // Verify O(N log N) duration grouping at 25,000-file archive scale
        std::vector<VideoFileInfo> synth(25000);
        std::vector<int> candidates(25000);
        for(size_t i=0;i<synth.size();++i) {
            candidates[i]=static_cast<int>(i);
            synth[i].fullPath=L"C:\\archive\\v_" + std::to_wstring(i) + L".mp4";
            synth[i].fileName=L"v_" + std::to_wstring(i) + L".mp4";
            synth[i].comparisonRoot=0;
            synth[i].fileSize=10000 + i;
            synth[i].metadataLoaded=true;
            // Pairs (0,1), (2,3), ..., (9998,9999) share duration within tolerance; 10000..24999 are spaced 50s apart
            if(i<10000) synth[i].duration100ns = 100000000LL + static_cast<int64_t>(i/2)*500000000LL + static_cast<int64_t>(i%2)*500000LL;
            else synth[i].duration100ns = 3000000000000LL + static_cast<int64_t>(i)*500000000LL;
        }
        ScanCriteria durCriteria;
        durCriteria.matchContent=false; durCriteria.matchSize=false; durCriteria.matchDuration=true;
        durCriteria.durationTol100ns=2000000LL;
        std::vector<VideoGroup> groups;
        std::atomic<bool> cancel{false};
        const auto t0=std::chrono::steady_clock::now();
        VideoScanner::GroupKnownFiles(synth,std::move(candidates),durCriteria,groups,cancel);
        const auto elapsedMs=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-t0).count();
        printf("25,000-file duration grouping: groups=%zu elapsed=%lldms (local_tier=%s)\n",groups.size(),static_cast<long long>(elapsedMs),StorageTierKey(static_cast<int>(localTier)));
        Check(groups.size()==5000 && elapsedMs<2000,"25k duration grouping accuracy and O(N log N) speed");
    }
    puts("PASS resources: CPU/memory/request/candidate bounds, 8K, HDD/SSD/UNC/unknown volume caps, and 25k O(N log N) duration grouping");

}

int wmain(int argc, wchar_t** argv) {
    Platform::ComInit com(Platform::ComApartment::ApartmentThreaded);
    Platform::MediaRuntime media;
    int result = 0;
    try {
        Check(com.succeeded() && media.succeeded(), "media runtime startup failed");
        Check(argc >= 2, "usage: CoreChecks --fileops [temporary-directory]");
        auto directory = std::filesystem::temp_directory_path() / (L"compareel-check-" + std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(directory);
        if (std::wstring(argv[1]) == L"--fileops") FileOperationChecks(directory);
        else if (std::wstring(argv[1]) == L"--transfers") TransferChecks(directory);
        else if (std::wstring(argv[1]) == L"--media" && argc >= 3) {
            if(argc>=5) MediaChecks(argv[2],std::stoul(argv[3]),std::stoul(argv[4]),!(argc>=6 && std::wstring(argv[5])==L"--no-artifacts")); else MediaChecks(argv[2]);
        }
        else if (std::wstring(argv[1]) == L"--matching") MatchingChecks(directory);
        else if (std::wstring(argv[1]) == L"--duration-filter") DurationPrefilterChecks();
        else if (std::wstring(argv[1]) == L"--resource-budget") ResourceBudgetChecks();
        else if (std::wstring(argv[1]) == L"--visual" && argc >= 6) VisualChecks(argv+2);
        else if (std::wstring(argv[1]) == L"--audio" && argc >= 3) AudioChecks(argv[2]);
        else if (std::wstring(argv[1]) == L"--short" && argc >= 3) ShortClipChecks(argv[2]);
        else if (std::wstring(argv[1]) == L"--scrub" && argc >= 3) ScrubChecks(argv[2]);
        else if (std::wstring(argv[1]) == L"--sampling-resume" && argc >= 3) SamplingResumeChecks(argv[2]);
#ifdef _WIN32
        else if (std::wstring(argv[1]) == L"--unsupported-codec" && argc >= 3) UnsupportedCodecChecks(argv[2]);
        else if (std::wstring(argv[1]) == L"--tracks" && argc >= 3) TrackProbe(argv[2]);
#endif
        else throw std::runtime_error("unknown check");
        std::filesystem::remove(directory);
    } catch (const std::exception& error) {
        fprintf(stderr, "FAIL %s\n", error.what());
        result = 1;
    }
    return result;
}
