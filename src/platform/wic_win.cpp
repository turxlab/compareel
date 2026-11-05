#include "platform/WinApi.h"
#include "platform/PlatformWic.h"
#include "platform/Platform.h"
#include <shlwapi.h>
#include <wincodec.h>
#include <wrl/client.h>

namespace Platform {

bool DecodeWicImage(const uint8_t* data,
                    size_t size,
                    std::atomic<bool>& cancel,
                    HRESULT& error,
                    WicDecodedImage& out) {
    out = {};
    if (cancel.load()) { error = HRESULT_FROM_WIN32(ERROR_CANCELLED); return false; }
    using Microsoft::WRL::ComPtr;
    ComInit com(ComApartment::MultiThreaded);

    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
        return false;
    }
    ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(reinterpret_cast<const BYTE*>(data), static_cast<UINT>(size)));
    if (!stream) return false;

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder))) {
        return false;
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return false;

    UINT w = 0, h = 0;
    if (FAILED(frame->GetSize(&w, &h)) || w == 0 || h == 0 || w > 16384 || h > 16384 || uint64_t(w) * h > 67108864) {
        return false;
    }

    int orientation = 1;
    ComPtr<IWICMetadataQueryReader> meta;
    if (SUCCEEDED(frame->GetMetadataQueryReader(&meta)) && meta) {
        PROPVARIANT var{};
        PropVariantInit(&var);
        if (SUCCEEDED(meta->GetMetadataByName(L"/app1/ifd/{ushort=274}", &var)) ||
            SUCCEEDED(meta->GetMetadataByName(L"/ifd/{ushort=274}", &var))) {
            if (var.vt == VT_UI2 && var.uiVal >= 1 && var.uiVal <= 8) orientation = var.uiVal;
            PropVariantClear(&var);
        }
    }

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter))) return false;
    if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
        return false;
    }

    const UINT stride = w * 4;
    const UINT bufSize = stride * h;
    try {
        out.bgra.resize(bufSize);
    } catch (const std::bad_alloc&) {
        error = E_OUTOFMEMORY;
        return false;
    }
    if (FAILED(converter->CopyPixels(nullptr, stride, bufSize, out.bgra.data()))) {
        out.bgra.clear();
        return false;
    }

    out.width = w;
    out.height = h;
    out.orientation = orientation;
    return true;
}

} // namespace Platform
