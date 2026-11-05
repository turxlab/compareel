#pragma once
#include "Common.h"

namespace Platform {

// Media Foundation result codes (mferror.h) reported by the native decoder; the values are fixed.
inline constexpr HRESULT kErrorInvalidMediaType = static_cast<HRESULT>(0xC00D36B4u); // MF_E_INVALIDMEDIATYPE
inline constexpr HRESULT kErrorInvalidTimestamp = static_cast<HRESULT>(0xC00D36C0u); // MF_E_INVALID_TIMESTAMP
inline constexpr HRESULT kErrorCodecNotFound = static_cast<HRESULT>(0xC00D5212u); // MF_E_TOPO_CODEC_NOT_FOUND
inline constexpr HRESULT kErrorUnsupportedByteStream = static_cast<HRESULT>(0xC00D36C4u); // MF_E_UNSUPPORTED_BYTESTREAM_TYPE

enum class NativeMediaResult {
    Succeeded,
    FallbackToFFmpeg,
    HardFailed,
    Cancelled
};

NativeMediaResult TryExtractNativeMetadata(VideoFileInfo& info,
                                           std::atomic<bool>* cancel,
                                           HRESULT& outError,
                                           bool& outMarkLoadedOnFailure);

NativeMediaResult TryExtractNativeFrames(VideoFileInfo& info,
                                         bool sampleVideo,
                                         bool preferFFmpeg,
                                         std::atomic<bool>* cancel,
                                         HRESULT& outError,
                                         bool& outCorruptedOnFailure,
                                         bool& outMarkLoadedOnFailure);

class NativeVideoSample {
public:
    NativeVideoSample() = default;
    explicit NativeVideoSample(void* sample);
    ~NativeVideoSample();
    NativeVideoSample(const NativeVideoSample& other);
    NativeVideoSample& operator=(const NativeVideoSample& other);
    NativeVideoSample(NativeVideoSample&& other) noexcept;
    NativeVideoSample& operator=(NativeVideoSample&& other) noexcept;

    explicit operator bool() const { return sample_ != nullptr; }
    void* raw() const { return sample_; }
    void reset();

private:
    void* sample_ = nullptr;
};

enum class NativeOpenStatus {
    Opened,
    FallbackToFFmpeg,
    InvalidDimensions,
    InvalidMediaType
};

struct NativeAudioFormat {
    bool available = false;
    uint32_t sampleRate = 0;
    uint32_t channels = 0;
    uint32_t bitsPerSample = 0;
};

struct NativeReadResult {
    HRESULT hr = S_OK;
    bool endOfStream = false;
    bool streamError = false;
    bool mediaTypeInvalid = false;
    bool isVideoStream = false;
    int64_t timestamp100ns = 0;
    NativeVideoSample sample;
};

class NativeVideoDecoder {
public:
    NativeVideoDecoder();
    ~NativeVideoDecoder();
    NativeVideoDecoder(const NativeVideoDecoder&) = delete;
    NativeVideoDecoder& operator=(const NativeVideoDecoder&) = delete;

    NativeOpenStatus Open(const std::wstring& path,
                          int64_t& outDuration100ns,
                          uint32_t& outWidth,
                          uint32_t& outHeight,
                          NativeAudioFormat& outAudio);
    void Close();

    NativeReadResult ReadNextSample(bool videoOnly, bool includeAudio);
    HRESULT FlushAndSeek(int64_t position100ns);
    bool DecodeVideoFrame(const NativeVideoSample& sample,
                          uint32_t& outWidth,
                          uint32_t& outHeight,
                          std::vector<uint8_t>& outBgra) const;
    void WithAudioPcm(const NativeVideoSample& sample,
                      const std::function<void(const uint8_t* data, uint32_t size)>& consumer) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace Platform
