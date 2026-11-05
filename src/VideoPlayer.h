#pragma once
#include "Common.h"
#include "platform/PlatformAudioOut.h"
#include <condition_variable>

struct VideoFrame {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> pixels;
};

class VideoPlayer {
public:
    VideoPlayer();
    ~VideoPlayer();

    // Load a video file and immediately decode/display its first frame (paused)
    bool Load(const std::wstring& filePath, std::shared_ptr<VideoPlayer> previousPlayer = nullptr);
    void ClosePrevious(std::shared_ptr<VideoPlayer> previousPlayer);

    // Immediately stop playback, join worker thread, and release file handle (for safe deletion)
    void Close();
    void RequestStop();

    void Play();
    void PlayAt(std::chrono::steady_clock::time_point start, int64_t mediaTime100ns);
    void Pause();
    void TogglePlayPause();
    void Stop(); // Pause and seek to 0

    // Non-blocking coalesced seek (works both while playing and while paused)
    void SeekTo(int64_t pos100ns);
    void SeekRatio(double ratio0to1);
    void ScrubTo(int64_t pos100ns);
    void ScrubRatio(double ratio0to1);
    void StepFrame(bool forward);

    void SetMuted(bool muted);
    bool IsMuted() const { return isMuted_.load(std::memory_order_relaxed); }

    bool IsLoaded() const { return isLoaded_.load(std::memory_order_relaxed); }
    bool IsPlaying() const { return isPlaying_.load(std::memory_order_relaxed); }
    bool IsEof() const { return atEof_.load(std::memory_order_relaxed); }

    int64_t GetDuration100ns() const { return duration100ns_.load(std::memory_order_relaxed); }
    int64_t GetPosition100ns() const { return currentPos100ns_.load(std::memory_order_relaxed); }
    uint32_t GetVideoWidth() const { return videoWidth_.load(std::memory_order_relaxed); }
    uint32_t GetVideoHeight() const { return videoHeight_.load(std::memory_order_relaxed); }
    const std::wstring& GetFilePath() const { return currentPath_; }
    std::shared_ptr<const VideoFrame> GetFrame() const;
    bool IsSeeking() const { return seeking_.load() || pendingSeek100ns_.load() >= 0; }
    HRESULT GetError() const { return error_.load(); }
    Platform::AudioError GetAudioError() const { return audioError_.load(); }
    int64_t GetLastSubmittedAudioTime100ns() const { return lastAudioTime100ns_.load(); }

private:
    void WorkerLoop(std::wstring path, std::shared_ptr<VideoPlayer> previousPlayer);
    void FFmpegLoop(const std::wstring& path);

    std::wstring currentPath_;

    std::thread             workerThread_;
    std::mutex              stateMutex_;
    std::condition_variable cv_;

    std::atomic<bool>     stopThread_{false};
    std::atomic<bool>     isLoaded_{false};
    std::atomic<bool>     isPlaying_{false};
    std::atomic<bool>     isMuted_{false};
    std::atomic<bool>     atEof_{false};
    std::atomic<int64_t>  pendingSeek100ns_{-1};
    std::atomic<int64_t>  duration100ns_{0};
    std::atomic<int64_t>  currentPos100ns_{0};
    std::atomic<uint32_t> videoWidth_{0};
    std::atomic<uint32_t> videoHeight_{0};
    std::atomic<bool> seeking_{false};
    std::atomic<HRESULT> error_{S_OK};
    std::atomic<Platform::AudioError> audioError_{Platform::kAudioErrorNone};
    std::atomic<int64_t> lastAudioTime100ns_{-1};
    std::atomic<int64_t> sharedStartNs_{0};
    std::atomic<int64_t> sharedMedia100ns_{0};
    std::atomic<uint64_t> clockRevision_{0};
    std::atomic<int64_t> lastExactTarget100ns_{-1};
    bool seekPreviousFrame_=false; // guarded by stateMutex_
    bool seekExact_=true; // guarded by stateMutex_

    mutable std::mutex frameMutex_;
    std::shared_ptr<const VideoFrame> frame_;
};
