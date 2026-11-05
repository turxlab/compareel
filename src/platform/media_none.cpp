// Platforms without a native media framework backend: every native request defers to the FFmpeg path,
// which the scanner and the player already use whenever the native decoder declines a file.
#include "platform/PlatformMedia.h"

namespace Platform {

// FFmpeg is the only decoder here, so when it fails the file is unreadable: that error and state are what the
// scanner records then, as the Windows backend reports its own decoder failure.
NativeMediaResult TryExtractNativeMetadata(VideoFileInfo&, std::atomic<bool>*, HRESULT& outError, bool& outMarkLoadedOnFailure) {
    outError = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    outMarkLoadedOnFailure = true;
    return NativeMediaResult::FallbackToFFmpeg;
}

NativeMediaResult TryExtractNativeFrames(VideoFileInfo&, bool, bool, std::atomic<bool>*, HRESULT& outError,
                                         bool& outCorruptedOnFailure, bool& outMarkLoadedOnFailure) {
    outError = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    outCorruptedOnFailure = true;
    outMarkLoadedOnFailure = true;
    return NativeMediaResult::FallbackToFFmpeg;
}

NativeVideoSample::NativeVideoSample(void*) {}
NativeVideoSample::~NativeVideoSample() = default;
NativeVideoSample::NativeVideoSample(const NativeVideoSample&) {}
NativeVideoSample& NativeVideoSample::operator=(const NativeVideoSample&) { return *this; }
NativeVideoSample::NativeVideoSample(NativeVideoSample&&) noexcept {}
NativeVideoSample& NativeVideoSample::operator=(NativeVideoSample&&) noexcept { return *this; }
void NativeVideoSample::reset() {}

struct NativeVideoDecoder::Impl {};

NativeVideoDecoder::NativeVideoDecoder() = default;
NativeVideoDecoder::~NativeVideoDecoder() = default;

NativeOpenStatus NativeVideoDecoder::Open(const std::wstring&, int64_t&, uint32_t&, uint32_t&, NativeAudioFormat& outAudio) {
    outAudio = {};
    return NativeOpenStatus::FallbackToFFmpeg;
}
void NativeVideoDecoder::Close() {}
NativeReadResult NativeVideoDecoder::ReadNextSample(bool, bool) {
    NativeReadResult result;
    result.hr = E_NOTIMPL;
    result.endOfStream = true;
    return result;
}
HRESULT NativeVideoDecoder::FlushAndSeek(int64_t) { return E_NOTIMPL; }
bool NativeVideoDecoder::DecodeVideoFrame(const NativeVideoSample&, uint32_t&, uint32_t&, std::vector<uint8_t>&) const { return false; }
void NativeVideoDecoder::WithAudioPcm(const NativeVideoSample&, const std::function<void(const uint8_t*, uint32_t)>&) const {}

} // namespace Platform
