#include "platform/WinApi.h"
#include "platform/PlatformMedia.h"
#include "Common.h"
#include <wrl/client.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

template <class T>
void SafeRelease(T** ppT) {
    if (*ppT) {
        (*ppT)->Release();
        *ppT = nullptr;
    }
}

// Media Foundation frame layout helpers (Windows only).
struct VideoFrameFormat {
    uint32_t surfaceWidth = 0, surfaceHeight = 0;
    uint32_t width = 0, height = 0, x = 0, y = 0;
    int32_t stride = 0;
};

bool NeedsFFmpegPresentation(IMFSourceReader* reader);
bool ReadVideoFrameFormat(IMFSourceReader* reader, VideoFrameFormat& format);
bool CopyVideoSampleBGRA(IMFSample* sample, const VideoFrameFormat& format, std::vector<uint8_t>& pixels);

#include "PerceptualHash.h"
#include <wrl/client.h>

bool NeedsFFmpegPresentation(IMFSourceReader* reader) {
    Microsoft::WRL::ComPtr<IMFMediaType> type;
    if (FAILED(reader->GetNativeMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &type))) return false;
    const auto rotation = MFGetAttributeUINT32(type.Get(), MF_MT_VIDEO_ROTATION, 0);
    const auto transfer = MFGetAttributeUINT32(type.Get(), MF_MT_TRANSFER_FUNCTION, 0);
    // The Media Foundation H.264 decoder handles 8-bit 4:2:0 only; on High 10/4:2:2/4:4:4 streams ReadSample can block forever.
    GUID subtype{};
    if (SUCCEEDED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) && subtype == MFVideoFormat_H264) {
        const auto profile = MFGetAttributeUINT32(type.Get(), MF_MT_MPEG2_PROFILE, 0);
        if (profile == 110 || profile == 122 || profile == 244 || profile == 44) return true;
    }
    return rotation != 0 || transfer == MFVideoTransFunc_2084 || transfer == MFVideoTransFunc_HLG;
}

bool ReadVideoFrameFormat(IMFSourceReader* reader, VideoFrameFormat& format) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IMFMediaType> type;
    if (FAILED(reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &type))) return false;
    GUID subtype{};
    if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) || subtype != MFVideoFormat_RGB32) return false;
    VideoFrameFormat next;
    if (FAILED(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &next.surfaceWidth, &next.surfaceHeight))) return false;
    if (!next.surfaceWidth || !next.surfaceHeight || next.surfaceWidth > 16384 || next.surfaceHeight > 16384) return false;
    UINT32 stride = 0;
    if (SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride))) next.stride = static_cast<int32_t>(stride);
    else {
        LONG computed = 0;
        if (FAILED(MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1, next.surfaceWidth, &computed))) return false;
        next.stride = computed;
    }
    next.width = next.surfaceWidth;
    next.height = next.surfaceHeight;
    MFVideoArea area{};
    UINT32 bytes = 0;
    bool aperture = SUCCEEDED(type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE,
        reinterpret_cast<UINT8*>(&area), sizeof(area), &bytes)) && bytes == sizeof(area);
    if (!aperture) aperture = SUCCEEDED(type->GetBlob(MF_MT_GEOMETRIC_APERTURE,
        reinterpret_cast<UINT8*>(&area), sizeof(area), &bytes)) && bytes == sizeof(area);
    if (aperture) {
        if (area.OffsetX.value < 0 || area.OffsetY.value < 0 || area.Area.cx <= 0 || area.Area.cy <= 0) return false;
        next.x = area.OffsetX.value;
        next.y = area.OffsetY.value;
        next.width = area.Area.cx;
        next.height = area.Area.cy;
    } else {
        // Decoder surfaces may be padded even when the encoded image width is not.
        ComPtr<IMFMediaType> native;
        UINT32 width = 0, height = 0;
        if (SUCCEEDED(reader->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &native)) &&
            SUCCEEDED(MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &width, &height)) && width && height) {
            next.width = std::min(width, next.surfaceWidth);
            next.height = std::min(height, next.surfaceHeight);
        }
    }
    if (uint64_t(next.x) + next.width > next.surfaceWidth || uint64_t(next.y) + next.height > next.surfaceHeight) return false;
    if (uint64_t(next.width) * next.height * 4 > 512ULL * 1024 * 1024) return false;
    format = next;
    return true;
}

bool CopyVideoSampleBGRA(IMFSample* sample, const VideoFrameFormat& format,
                         std::vector<uint8_t>& pixels) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IMFMediaBuffer> buffer;
    DWORD count = 0;
    if (!sample || FAILED(sample->GetBufferCount(&count)) || !count) return false;
    if (FAILED(sample->GetBufferByIndex(0, &buffer))) return false;
    auto copyRows = [&](const BYTE* top, LONG pitch) {
        if (!top || std::abs(int64_t(pitch)) < int64_t(format.surfaceWidth) * 4) return false;
        size_t rowBytes = size_t(format.width) * 4;
        pixels.resize(rowBytes * format.height);
        for (uint32_t y = 0; y < format.height; ++y) {
            auto row = top + int64_t(y + format.y) * pitch + size_t(format.x) * 4;
            memcpy(pixels.data() + size_t(y) * rowBytes, row, rowBytes);
            // MF RGB32's unused byte is undefined; QImage RGB32 requires opaque alpha.
            for (size_t x = 3; x < rowBytes; x += 4) pixels[size_t(y) * rowBytes + x] = 255;
        }
        return true;
    };
    ComPtr<IMF2DBuffer> image;
    if (count == 1 && SUCCEEDED(buffer.As(&image))) {
        BYTE* top = nullptr;
        LONG pitch = 0;
        if (FAILED(image->Lock2D(&top, &pitch))) return false;
        bool copied = copyRows(top, pitch);
        image->Unlock2D();
        return copied;
    }
    if (count > 1 && FAILED(sample->ConvertToContiguousBuffer(&buffer))) return false;
    BYTE* start = nullptr;
    DWORD length = 0;
    if (FAILED(buffer->Lock(&start, nullptr, &length))) return false;
    int64_t pitch = format.stride;
    uint64_t required = uint64_t(std::abs(pitch)) * (format.surfaceHeight - 1) + uint64_t(format.surfaceWidth) * 4;
    bool copied = false;
    if (start && length >= required) {
        const BYTE* top = pitch < 0 ? start + (format.surfaceHeight - 1) * (-pitch) : start;
        copied = copyRows(top, format.stride);
    }
    buffer->Unlock();
    return copied;
}

namespace Platform {
namespace {

HRESULT CreateScanReader(const std::wstring& path, bool advanced, IMFSourceReader** output) {
    using Microsoft::WRL::ComPtr;
    *output = nullptr;
    ComPtr<IMFAttributes> attributes;
    ComPtr<IMFSourceReader> reader;
    ComPtr<IMFMediaType> type;
    HRESULT hr = MFCreateAttributes(&attributes, 2);
    if (SUCCEEDED(hr)) hr = attributes->SetUINT32(advanced ? MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING : MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    if (SUCCEEDED(hr)) hr = attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    if (SUCCEEDED(hr)) hr = MFCreateSourceReaderFromURL(path.c_str(), attributes.Get(), &reader);
    if (SUCCEEDED(hr)) hr = reader->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    if (SUCCEEDED(hr)) hr = reader->SetStreamSelection(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);
    if (SUCCEEDED(hr)) hr = MFCreateMediaType(&type);
    if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    if (SUCCEEDED(hr)) hr = reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, type.Get());
    if (SUCCEEDED(hr)) *output = reader.Detach();
    return hr;
}

// Decode the first presented frame at or after the requested media time.
bool ReadOneFrameBGRA(IMFSourceReader* reader, uint32_t& width, uint32_t& height, int32_t& stride,
                      FrameSignature& signature, std::vector<uint8_t>* thumbnail, int64_t target = 0,
                      std::atomic<bool>* cancel = nullptr, bool orientations = false, std::vector<uint8_t>* scratch = nullptr) {
    VideoFrameFormat format;
    if (!ReadVideoFrameFormat(reader, format)) return false;
    signature = {};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    for (int attempt = 0; attempt < 100000 && (!cancel || !cancel->load()) && std::chrono::steady_clock::now() < deadline; ++attempt) {
        DWORD stream = 0, flags = 0;
        LONGLONG timestamp = 0;
        Microsoft::WRL::ComPtr<IMFSample> sample;
        HRESULT hr = reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &stream, &flags, &timestamp, &sample);
        if (FAILED(hr) || (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR))) return false;
        if ((flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) && !ReadVideoFrameFormat(reader, format)) return false;
        if (!sample || timestamp < target) continue;
        std::vector<uint8_t> localPixels;
        auto& pixels = scratch ? *scratch : localPixels;
        if (!CopyVideoSampleBGRA(sample.Get(), format, pixels)) return false;
        width = format.width;
        height = format.height;
        stride = static_cast<int32_t>(width * 4);
        signature = PerceptualHash::ComputeSignatureFromBGRA(pixels.data(), width, height, stride);
        signature.timestamp100ns = timestamp;
        if (orientations) PerceptualHash::AddNormalizedOrientations(pixels.data(), width, height, stride, signature);
        if (thumbnail) PerceptualHash::GenerateThumbnailBGRA(pixels.data(), width, height, stride, *thumbnail);
        return signature.valid;
    }
    return false;
}

} // namespace

NativeMediaResult TryExtractNativeMetadata(VideoFileInfo& info,
                                           std::atomic<bool>* cancel,
                                           HRESULT& outError,
                                           bool& outMarkLoadedOnFailure) {
    outError = S_OK;
    outMarkLoadedOnFailure = true;

    IMFAttributes* pAttr = nullptr;
    MFCreateAttributes(&pAttr, 1);

    IMFSourceReader* pReader = nullptr;
    HRESULT hr = MFCreateSourceReaderFromURL(info.fullPath.c_str(), pAttr, &pReader);
    SafeRelease(&pAttr);

    if (FAILED(hr) || !pReader) {
        outError = FAILED(hr) ? hr : E_FAIL;
        outMarkLoadedOnFailure = true;
        return NativeMediaResult::FallbackToFFmpeg;
    }
    if (NeedsFFmpegPresentation(pReader)) {
        SafeRelease(&pReader);
        outError = MF_E_INVALIDMEDIATYPE;
        outMarkLoadedOnFailure = false;
        return NativeMediaResult::FallbackToFFmpeg;
    }

    // Duration
    PROPVARIANT var{};
    PropVariantInit(&var);
    if (SUCCEEDED(pReader->GetPresentationAttribute(
            static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE),
            MF_PD_DURATION,
            &var))) {
        if (var.vt == VT_UI8) {
            info.duration100ns = static_cast<int64_t>(var.uhVal.QuadPart);
        } else if (var.vt == VT_I8) {
            info.duration100ns = var.hVal.QuadPart;
        }
        PropVariantClear(&var);
    }

    // Resolution from native video media type
    IMFMediaType* pNativeType = nullptr;
    if (SUCCEEDED(pReader->GetNativeMediaType(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
            0,
            &pNativeType)) && pNativeType) {
        UINT32 w = 0, h = 0;
        if (SUCCEEDED(MFGetAttributeSize(pNativeType, MF_MT_FRAME_SIZE, &w, &h))) {
            info.width = w;
            info.height = h;
        }
        UINT32 numerator = 0, denominator = 0;
        if (SUCCEEDED(MFGetAttributeRatio(pNativeType, MF_MT_FRAME_RATE, &numerator, &denominator)) && denominator)
            info.fps = double(numerator) / denominator;
        info.videoBitrate = MFGetAttributeUINT32(pNativeType, MF_MT_AVG_BITRATE, 0);
        GUID subtype{};
        if (SUCCEEDED(pNativeType->GetGUID(MF_MT_SUBTYPE, &subtype))) {
            for (int i = 0; i < 4; ++i) {
                wchar_t ch = static_cast<wchar_t>((subtype.Data1 >> (8 * i)) & 255);
                if (ch >= 32 && ch <= 126) info.codec += ch;
            }
        }
        SafeRelease(&pNativeType);
    }

    IMFMediaType* audio = nullptr;
    HRESULT audioResult = pReader->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, &audio);
    if (SUCCEEDED(audioResult) && audio) {
        info.audioChannels = int(MFGetAttributeUINT32(audio, MF_MT_AUDIO_NUM_CHANNELS, 0));
        info.audioSampleRate = MFGetAttributeUINT32(audio, MF_MT_AUDIO_SAMPLES_PER_SECOND, 0);
        GUID subtype{};
        audio->GetGUID(MF_MT_SUBTYPE, &subtype);
        if (subtype == MFAudioFormat_AAC) info.audioCodec = L"AAC";
        else if (subtype == MFAudioFormat_MP3) info.audioCodec = L"MP3";
        else if (subtype == MFAudioFormat_PCM) info.audioCodec = L"PCM";
        else if (subtype == MFAudioFormat_Float) info.audioCodec = L"Float PCM";
        else info.audioCodec = L"Other";
        SafeRelease(&audio);
    } else if (audioResult == MF_E_INVALIDSTREAMNUMBER) {
        info.audioChannels = 0;
    }
    SafeRelease(&pReader);
    if (cancel && cancel->load()) return NativeMediaResult::Cancelled;
    if (info.width == 0 || info.height == 0) {
        outError = MF_E_INVALIDMEDIATYPE;
        outMarkLoadedOnFailure = true;
        return NativeMediaResult::FallbackToFFmpeg;
    }
    info.metadataLoaded = true;
    info.corrupted = false;
    info.previewFailed = false;
    return NativeMediaResult::Succeeded;
}

NativeMediaResult TryExtractNativeFrames(VideoFileInfo& info,
                                         bool sampleVideo,
                                         bool preferFFmpeg,
                                         std::atomic<bool>* cancel,
                                         HRESULT& outError,
                                         bool& outCorruptedOnFailure,
                                         bool& outMarkLoadedOnFailure) {
    outError = S_OK;
    outCorruptedOnFailure = true;
    outMarkLoadedOnFailure = true;

    IMFSourceReader* pReader = nullptr;
    HRESULT hr = CreateScanReader(info.fullPath, true, &pReader);
    bool advanced = SUCCEEDED(hr);
    // Preserve codec compatibility without keeping two readers open or changing sample positions.
    if (FAILED(hr)) hr = CreateScanReader(info.fullPath, false, &pReader);

    if (FAILED(hr) || !pReader || preferFFmpeg || NeedsFFmpegPresentation(pReader)) {
        SafeRelease(&pReader);
        outError = FAILED(hr) ? hr : E_FAIL;
        outCorruptedOnFailure = true;
        outMarkLoadedOnFailure = true;
        return NativeMediaResult::FallbackToFFmpeg;
    }

    // Get Duration
    PROPVARIANT var{};
    PropVariantInit(&var);
    if (SUCCEEDED(pReader->GetPresentationAttribute(
            static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE),
            MF_PD_DURATION,
            &var))) {
        if (var.vt == VT_UI8) {
            info.duration100ns = static_cast<int64_t>(var.uhVal.QuadPart);
        } else if (var.vt == VT_I8) {
            info.duration100ns = var.hVal.QuadPart;
        }
        PropVariantClear(&var);
    }

    int32_t defaultStride = 0;
    IMFMediaType* pCurrentType = nullptr;
    if (SUCCEEDED(pReader->GetCurrentMediaType(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
            &pCurrentType)) && pCurrentType) {
        UINT32 w = 0, h = 0;
        if (SUCCEEDED(MFGetAttributeSize(pCurrentType, MF_MT_FRAME_SIZE, &w, &h))) {
            info.width = w;
            info.height = h;
        }
        UINT32 uStride = 0;
        if (SUCCEEDED(pCurrentType->GetUINT32(MF_MT_DEFAULT_STRIDE, &uStride))) {
            defaultStride = static_cast<int32_t>(uStride);
        }
        SafeRelease(&pCurrentType);
    }

    if (info.width == 0 || info.height == 0) {
        SafeRelease(&pReader);
        outError = MF_E_INVALIDMEDIATYPE;
        outCorruptedOnFailure = true;
        outMarkLoadedOnFailure = true;
        return NativeMediaResult::FallbackToFFmpeg;
    }
    info.metadataLoaded = true;

    std::vector<uint8_t> scratch;
    // Reuse the RGB buffer across seeks; frame timestamps and image evidence are unchanged.
    bool first = ReadOneFrameBGRA(pReader, info.width, info.height, defaultStride, info.sigFirst, &info.thumbBGRA, 0, cancel, false, &scratch);
    if (!first && advanced && (!cancel || !cancel->load())) {
        SafeRelease(&pReader);
        hr = CreateScanReader(info.fullPath, false, &pReader);
        if (SUCCEEDED(hr)) first = ReadOneFrameBGRA(pReader, info.width, info.height, defaultStride, info.sigFirst, &info.thumbBGRA, 0, cancel, false, &scratch);
    }
    if (!first && (!cancel || !cancel->load())) {
        SafeRelease(&pReader);
        outError = FAILED(hr) ? hr : MF_E_INVALIDMEDIATYPE;
        outCorruptedOnFailure = false;
        outMarkLoadedOnFailure = false;
        return NativeMediaResult::FallbackToFFmpeg;
    }

    // If first frame is solid black/white (lowVariance) and video is > 0.3s long, also sample at ~0.5s
    if (info.sigFirst.valid && info.sigFirst.lowVariance && info.duration100ns > 3000000LL) {
        LONGLONG seekTarget = std::min<LONGLONG>(5000000LL, info.duration100ns / 3);
        PROPVARIANT varSeek{};
        PropVariantInit(&varSeek);
        varSeek.vt = VT_I8;
        varSeek.hVal.QuadPart = seekTarget;
        if (SUCCEEDED(pReader->SetCurrentPosition(GUID_NULL, varSeek))) {
            std::vector<uint8_t> altThumb;
            if (ReadOneFrameBGRA(pReader, info.width, info.height, defaultStride, info.sigAlt, &altThumb, seekTarget, cancel, false, &scratch)) {
                if (!info.sigAlt.lowVariance && !altThumb.empty()) {
                    info.thumbBGRA = std::move(altThumb);
                }
            }
        }
        PropVariantClear(&varSeek);
    }

    if (sampleVideo && info.duration100ns > 0) {
        info.samples.clear();
        std::set<int64_t> timestamps;
        for (int position : {10, 30, 50, 70, 90}) {
            if (cancel && cancel->load()) break;
            int64_t target = info.duration100ns * position / 100;
            PROPVARIANT seek{};
            seek.vt = VT_I8;
            seek.hVal.QuadPart = target;
            FrameSignature signature;
            std::vector<uint8_t> thumbnail;
            if (SUCCEEDED(pReader->SetCurrentPosition(GUID_NULL, seek)))
                ReadOneFrameBGRA(pReader, info.width, info.height, defaultStride, signature,
                    &thumbnail, target, cancel, true, &scratch);
            if (signature.lowVariance || !timestamps.insert(signature.timestamp100ns).second) signature.valid = false;
            info.samples.push_back(signature);
            if (position == 50 && signature.valid) info.thumbBGRA = std::move(thumbnail);
        }
    }

    SafeRelease(&pReader);
    if (cancel && cancel->load()) {
        // A resumed scan must retry interrupted evidence instead of keeping a partial sample set.
        info.samples.clear();
        info.frameLoaded = false;
        return NativeMediaResult::Cancelled;
    }
    return NativeMediaResult::Succeeded;
}

NativeVideoSample::NativeVideoSample(void* sample) : sample_(sample) {
    if (sample_) static_cast<IMFSample*>(sample_)->AddRef();
}

NativeVideoSample::~NativeVideoSample() {
    reset();
}

NativeVideoSample::NativeVideoSample(const NativeVideoSample& other) : sample_(other.sample_) {
    if (sample_) static_cast<IMFSample*>(sample_)->AddRef();
}

NativeVideoSample& NativeVideoSample::operator=(const NativeVideoSample& other) {
    if (this != &other) {
        if (other.sample_) static_cast<IMFSample*>(other.sample_)->AddRef();
        reset();
        sample_ = other.sample_;
    }
    return *this;
}

NativeVideoSample::NativeVideoSample(NativeVideoSample&& other) noexcept : sample_(other.sample_) {
    other.sample_ = nullptr;
}

NativeVideoSample& NativeVideoSample::operator=(NativeVideoSample&& other) noexcept {
    if (this != &other) {
        reset();
        sample_ = other.sample_;
        other.sample_ = nullptr;
    }
    return *this;
}

void NativeVideoSample::reset() {
    auto* rawSample = static_cast<IMFSample*>(sample_);
    SafeRelease(&rawSample);
    sample_ = nullptr;
}

struct NativeVideoDecoder::Impl {
    IMFSourceReader* reader = nullptr;
    VideoFrameFormat frameFormat;
    DWORD actualVideoStreamIndex = 0;
    ~Impl() { SafeRelease(&reader); }
};

NativeVideoDecoder::NativeVideoDecoder() : impl_(std::make_unique<Impl>()) {}
NativeVideoDecoder::~NativeVideoDecoder() { Close(); }

void NativeVideoDecoder::Close() {
    if (impl_) SafeRelease(&impl_->reader);
}

NativeOpenStatus NativeVideoDecoder::Open(const std::wstring& path,
                                          int64_t& outDuration100ns,
                                          uint32_t& outWidth,
                                          uint32_t& outHeight,
                                          NativeAudioFormat& outAudio) {
    Close();
    outDuration100ns = 0;
    outWidth = 0;
    outHeight = 0;
    outAudio = {};

    IMFAttributes* pAttr = nullptr;
    MFCreateAttributes(&pAttr, 2);
    if (pAttr) {
        pAttr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        pAttr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    }

    IMFSourceReader* pReader = nullptr;
    HRESULT hr = MFCreateSourceReaderFromURL(path.c_str(), pAttr, &pReader);
    SafeRelease(&pAttr);

    if (FAILED(hr) || !pReader) return NativeOpenStatus::FallbackToFFmpeg;
    if (NeedsFFmpegPresentation(pReader)) {
        SafeRelease(&pReader);
        return NativeOpenStatus::FallbackToFFmpeg;
    }

    PROPVARIANT varDur{};
    PropVariantInit(&varDur);
    if (SUCCEEDED(pReader->GetPresentationAttribute(
            static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE),
            MF_PD_DURATION,
            &varDur))) {
        if (varDur.vt == VT_UI8) outDuration100ns = static_cast<int64_t>(varDur.uhVal.QuadPart);
        else if (varDur.vt == VT_I8) outDuration100ns = varDur.hVal.QuadPart;
        PropVariantClear(&varDur);
    }

    pReader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    pReader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);

    IMFMediaType* pVidType = nullptr;
    MFCreateMediaType(&pVidType);
    if (pVidType) {
        pVidType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        pVidType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        hr = pReader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, pVidType);
        SafeRelease(&pVidType);
    }
    if (FAILED(hr)) {
        SafeRelease(&pReader);
        return NativeOpenStatus::FallbackToFFmpeg;
    }

    IMFMediaType* pCurVid = nullptr;
    if (SUCCEEDED(pReader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &pCurVid)) && pCurVid) {
        MFGetAttributeSize(pCurVid, MF_MT_FRAME_SIZE, &outWidth, &outHeight);
        SafeRelease(&pCurVid);
    }
    if (outWidth == 0 || outHeight == 0) {
        SafeRelease(&pReader);
        return NativeOpenStatus::InvalidDimensions;
    }

    if (SUCCEEDED(pReader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), TRUE))) {
        IMFMediaType* pAudType = nullptr;
        MFCreateMediaType(&pAudType);
        if (pAudType) {
            pAudType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            pAudType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
            pAudType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            if (SUCCEEDED(pReader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, pAudType))) {
                IMFMediaType* pCurAud = nullptr;
                if (SUCCEEDED(pReader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), &pCurAud)) && pCurAud) {
                    outAudio.available = true;
                    outAudio.sampleRate = MFGetAttributeUINT32(pCurAud, MF_MT_AUDIO_SAMPLES_PER_SECOND, 44100);
                    outAudio.channels = MFGetAttributeUINT32(pCurAud, MF_MT_AUDIO_NUM_CHANNELS, 2);
                    outAudio.bitsPerSample = MFGetAttributeUINT32(pCurAud, MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
                    SafeRelease(&pCurAud);
                }
            } else {
                pReader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), FALSE);
            }
            SafeRelease(&pAudType);
        }
    }

    if (!ReadVideoFrameFormat(pReader, impl_->frameFormat)) {
        SafeRelease(&pReader);
        return NativeOpenStatus::InvalidMediaType;
    }

    impl_->reader = pReader;
    impl_->actualVideoStreamIndex = 0;
    return NativeOpenStatus::Opened;
}

NativeReadResult NativeVideoDecoder::ReadNextSample(bool videoOnly, bool includeAudio) {
    NativeReadResult out;
    if (!impl_ || !impl_->reader) {
        out.hr = E_FAIL;
        out.streamError = true;
        return out;
    }
    const DWORD readStream = (videoOnly || !includeAudio)
        ? static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM)
        : static_cast<DWORD>(MF_SOURCE_READER_ANY_STREAM);
    DWORD stream = 0, flags = 0;
    LONGLONG timestamp = 0;
    IMFSample* sample = nullptr;
    out.hr = impl_->reader->ReadSample(readStream, 0, &stream, &flags, &timestamp, &sample);
    if (videoOnly) impl_->actualVideoStreamIndex = stream;
    out.isVideoStream = videoOnly || (stream == impl_->actualVideoStreamIndex);
    out.endOfStream = (flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0;
    out.streamError = FAILED(out.hr) || ((flags & MF_SOURCE_READERF_ERROR) != 0);
    if ((flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) && out.isVideoStream &&
        !ReadVideoFrameFormat(impl_->reader, impl_->frameFormat)) {
        out.mediaTypeInvalid = true;
    }
    out.timestamp100ns = timestamp;
    if (sample) {
        out.sample = NativeVideoSample(sample);
        SafeRelease(&sample);
    }
    return out;
}

HRESULT NativeVideoDecoder::FlushAndSeek(int64_t position100ns) {
    if (!impl_ || !impl_->reader) return E_FAIL;
    impl_->reader->Flush(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS));
    PROPVARIANT varPos{};
    PropVariantInit(&varPos);
    varPos.vt = VT_I8;
    varPos.hVal.QuadPart = std::max<int64_t>(0, position100ns);
    HRESULT hr = impl_->reader->SetCurrentPosition(GUID_NULL, varPos);
    PropVariantClear(&varPos);
    return hr;
}

bool NativeVideoDecoder::DecodeVideoFrame(const NativeVideoSample& sample,
                                          uint32_t& outWidth,
                                          uint32_t& outHeight,
                                          std::vector<uint8_t>& outBgra) const {
    if (!impl_ || !sample) return false;
    outWidth = impl_->frameFormat.width;
    outHeight = impl_->frameFormat.height;
    return CopyVideoSampleBGRA(static_cast<IMFSample*>(sample.raw()), impl_->frameFormat, outBgra);
}

void NativeVideoDecoder::WithAudioPcm(const NativeVideoSample& sample,
                                      const std::function<void(const uint8_t* data, uint32_t size)>& consumer) const {
    if (!sample) return;
    IMFMediaBuffer* pAudBuf = nullptr;
    if (SUCCEEDED(static_cast<IMFSample*>(sample.raw())->ConvertToContiguousBuffer(&pAudBuf)) && pAudBuf) {
        BYTE* pAudioBytes = nullptr;
        DWORD cbMax = 0, cbCur = 0;
        if (SUCCEEDED(pAudBuf->Lock(&pAudioBytes, &cbMax, &cbCur)) && pAudioBytes && cbCur > 0) {
            consumer(pAudioBytes, cbCur);
            pAudBuf->Unlock();
        }
        SafeRelease(&pAudBuf);
    }
}

} // namespace Platform
