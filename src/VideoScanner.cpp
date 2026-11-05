#include "VideoScanner.h"
#include "platform/PlatformMedia.h"
#include "FileOperations.h"
#include "FFmpegReader.h"

bool VideoScanner::ExtractMetadataOnly(VideoFileInfo& info, std::atomic<bool>* cancel) {
    if (cancel && cancel->load()) return false;
    if (info.metadataLoaded) return !info.corrupted;

    HRESULT nativeError = S_OK;
    bool markLoadedOnFailure = true;
    const auto status = Platform::TryExtractNativeMetadata(info, cancel, nativeError, markLoadedOnFailure);
    if (status == Platform::NativeMediaResult::Succeeded) return true;
    if (status == Platform::NativeMediaResult::Cancelled) return false;

    const bool hadTentativeCorrupted = info.metadataLoaded && info.corrupted;
    FFmpegReader fallback;
    if (fallback.Open(info.fullPath, cancel) && fallback.Metadata(info)) return true;
    if (cancel && cancel->load()) {
        if (hadTentativeCorrupted) {
            info.metadataLoaded = false;
            info.corrupted = false;
        }
        return false;
    }
    info.error = nativeError;
    info.corrupted = true;
    if (markLoadedOnFailure) info.metadataLoaded = true;
    return false;
}

bool VideoScanner::ExtractMetadataAndFrame(VideoFileInfo& info, bool sampleVideo, std::atomic<bool>* cancel) {
    if (cancel && cancel->load()) return false;
    if (info.frameLoaded && (!sampleVideo || info.samples.size() == 5)) return info.sigFirst.valid;

    auto verifyIdentity = [&]() -> bool {
        if (!info.identity.valid) return true;
        DWORD idErr = 0;
        const auto currentId = FileOperations::ReadIdentity(info.fullPath, &idErr);
        if (!currentId.valid || currentId != info.identity) {
            info.samples.clear();
            info.frameLoaded = false;
            info.corrupted = true;
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
            return verifyIdentity();
        }
        if (cancel && cancel->load()) return false;
        if (corruptedOnFailure) info.samples.clear();
        info.error = nativeError;
        if (corruptedOnFailure) info.corrupted = true;
        if (markLoadedOnFailure) info.metadataLoaded = true;
        info.frameLoaded = false;
        return false;
    }

    if (!verifyIdentity()) return false;
    info.frameLoaded = true;
    return info.sigFirst.valid;
}
