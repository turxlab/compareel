// macOS implementation of PlatformWic.h: ImageIO is the system image codec layer, as WIC is on Windows,
// and also develops camera RAW files that have no usable embedded preview.
#include "platform/PlatformWic.h"
#include "platform/Platform.h"
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <algorithm>
#include <new>

namespace Platform {
namespace {
template <typename T> struct CFRef {
    T ref = nullptr;
    explicit CFRef(T value) : ref(value) {}
    ~CFRef() { if (ref) CFRelease(ref); }
    CFRef(const CFRef&) = delete;
    CFRef& operator=(const CFRef&) = delete;
    explicit operator bool() const { return ref != nullptr; }
};

int64_t Number(CFDictionaryRef properties, CFStringRef key) {
    const auto value = static_cast<CFNumberRef>(CFDictionaryGetValue(properties, key));
    int64_t result = 0;
    if (!value || CFGetTypeID(value) != CFNumberGetTypeID() || !CFNumberGetValue(value, kCFNumberSInt64Type, &result)) return 0;
    return result;
}
}

bool DecodeWicImage(const uint8_t* data,
                    size_t size,
                    std::atomic<bool>& cancel,
                    HRESULT& error,
                    WicDecodedImage& out) {
    out = {};
    if (cancel.load()) { error = HRESULT_FROM_WIN32(ERROR_CANCELLED); return false; }
    error = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    CFRef<CFDataRef> bytes(CFDataCreateWithBytesNoCopy(nullptr, data, static_cast<CFIndex>(size), kCFAllocatorNull));
    if (!bytes) return false;
    CFRef<CGImageSourceRef> source(CGImageSourceCreateWithData(bytes.ref, nullptr));
    if (!source || CGImageSourceGetCount(source.ref) == 0) return false;

    // The size comes from the header, so oversized images are refused before anything is decoded.
    CFRef<CFDictionaryRef> properties(CGImageSourceCopyPropertiesAtIndex(source.ref, 0, nullptr));
    if (!properties) return false;
    const auto w = Number(properties.ref, kCGImagePropertyPixelWidth), h = Number(properties.ref, kCGImagePropertyPixelHeight);
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384 || uint64_t(w) * uint64_t(h) > 67108864) return false;
    const auto orientation = Number(properties.ref, kCGImagePropertyOrientation);

    CFRef<CGImageRef> image(CGImageSourceCreateImageAtIndex(source.ref, 0, nullptr));
    if (!image) { error = HRESULT_FROM_WIN32(ERROR_INVALID_DATA); return false; }
    if (cancel.load()) { error = HRESULT_FROM_WIN32(ERROR_CANCELLED); return false; }
    // RAW development may settle on another size than the header's.
    const auto width = CGImageGetWidth(image.ref), height = CGImageGetHeight(image.ref);
    if (width == 0 || height == 0 || width > 16384 || height > 16384 || uint64_t(width) * height > 67108864) return false;
    try {
        out.bgra.assign(width * height * 4, 0);
    } catch (const std::bad_alloc&) {
        error = E_OUTOFMEMORY;
        return false;
    }
    // Drawing into an sRGB context applies the embedded colour profile.
    CFRef<CGColorSpaceRef> srgb(CGColorSpaceCreateWithName(kCGColorSpaceSRGB));
    CFRef<CGContextRef> context(CGBitmapContextCreate(out.bgra.data(), width, height, 8, width * 4, srgb.ref,
        static_cast<uint32_t>(kCGImageAlphaPremultipliedFirst) | static_cast<uint32_t>(kCGBitmapByteOrder32Little)));
    if (!context) { out.bgra.clear(); return false; }
    CGContextSetBlendMode(context.ref, kCGBlendModeCopy);
    CGContextDrawImage(context.ref, CGRectMake(0, 0, CGFloat(width), CGFloat(height)), image.ref);
    // Core Graphics only draws premultiplied; the shared contract, like WIC's 32bppBGRA, is straight alpha.
    for (size_t i = 0; i < out.bgra.size(); i += 4) {
        const unsigned alpha = out.bgra[i + 3];
        if (alpha == 0 || alpha == 255) continue;
        for (size_t c = 0; c < 3; ++c) out.bgra[i + c] = static_cast<uint8_t>(std::min(255u, (out.bgra[i + c] * 255u + alpha / 2) / alpha));
    }
    out.width = static_cast<uint32_t>(width);
    out.height = static_cast<uint32_t>(height);
    out.orientation = orientation >= 1 && orientation <= 8 ? static_cast<int>(orientation) : 1;
    error = S_OK;
    return true;
}

} // namespace Platform
