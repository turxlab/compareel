#include "VideoScanner.h"
#include "platform/PlatformMedia.h"
#include "FileOperations.h"
#include "FFmpegReader.h"

bool VideoScanner::ExtractMetadataOnly(VideoFileInfo& info, std::atomic<bool>* cancel) {
    if (cancel && cancel->load()) return false;
    if (info.metadataLoaded) return !info.corrupted && !info.previewFailed;

    HRESULT nativeError = S_OK;
    bool markLoadedOnFailure = true;
    const auto status = Platform::TryExtractNativeMetadata(info, cancel, nativeError, markLoadedOnFailure);
    if (status == Platform::NativeMediaResult::Succeeded) {
        info.cachedDetailsVersion = 2;
        info.cachedImageRenderVersion = 0;
        return true;
    }
    if (status == Platform::NativeMediaResult::Cancelled) return false;

    FFmpegReader fallback;
    if (fallback.Open(info.fullPath, cancel) && fallback.Metadata(info)) {
        info.cachedDetailsVersion = 2;
        info.cachedImageRenderVersion = 0;
        return true;
    }
    if (cancel && cancel->load()) return false;

    info.duration100ns = 0;
    info.width = 0;
    info.height = 0;
    info.error = FAILED(nativeError) ? nativeError : E_FAIL;
    DWORD idErr = 0;
    const auto currentId = FileOperations::ReadIdentity(info.fullPath, &idErr);
    if (!currentId.valid || (info.identity.valid && currentId != info.identity)) {
        info.contentHash.clear();
        info.corrupted = true;
        info.previewFailed = false;
        info.error = HRESULT_FROM_WIN32(idErr ? idErr : ERROR_FILE_INVALID);
    } else {
        info.previewFailed = true;
        info.corrupted = false;
    }
    if (markLoadedOnFailure) {
        info.cachedDetailsVersion = 2;
        info.cachedImageRenderVersion = 0;
        info.metadataLoaded = true;
    }
    return false;
}

bool VideoScanner::ExtractMetadataAndFrame(VideoFileInfo& info, bool sampleVideo, std::atomic<bool>* cancel) {
    if (cancel && cancel->load()) return false;
    if (info.frameLoaded && (!sampleVideo || info.samples.size() == 5)) return info.sigFirst.valid;

    auto verifyIdentity = [&]() -> bool {
        DWORD idErr = 0;
        const auto currentId = FileOperations::ReadIdentity(info.fullPath, &idErr);
        if (!currentId.valid || (info.identity.valid && currentId != info.identity)) {
            info.contentHash.clear();
            info.samples.clear();
            info.frameLoaded = false;
            info.corrupted = true;
            info.previewFailed = false;
            info.error = HRESULT_FROM_WIN32(idErr ? idErr : ERROR_FILE_INVALID);
            return false;
        }
        return true;
    };

    HRESULT nativeError = S_OK;
    bool corruptedOnFailure = true;
    bool markLoadedOnFailure = true;
    const auto status = Platform::TryExtractNativeFrames(
        info, sampleVideo, FFmpegReader::GpuEnabled(), cancel,
        nativeError, corruptedOnFailure, markLoadedOnFailure);

    if (status == Platform::NativeMediaResult::HardFailed || status == Platform::NativeMediaResult::Cancelled) {
        return false;
    }
    if (status == Platform::NativeMediaResult::FallbackToFFmpeg) {
        if (ReadFFmpegSamples(info, sampleVideo, cancel)) {
            if (!verifyIdentity()) return false;
            info.cachedDetailsVersion = 2;
            info.cachedImageRenderVersion = 0;
            return true;
        }
        if (cancel && cancel->load()) return false;
        if (!verifyIdentity()) return false;
        if (corruptedOnFailure) info.samples.clear();
        info.error = FAILED(nativeError) ? nativeError : E_FAIL;
        info.previewFailed = true;
        info.corrupted = false;
        if (markLoadedOnFailure) {
            info.cachedDetailsVersion = 2;
            info.cachedImageRenderVersion = 0;
            info.metadataLoaded = true;
        }
        info.frameLoaded = false;
        return false;
    }

    if (!verifyIdentity()) return false;
    info.cachedDetailsVersion = 2;
    info.cachedImageRenderVersion = 0;
    info.frameLoaded = true;
    info.corrupted = false;
    info.previewFailed = false;
    return info.sigFirst.valid;
}
