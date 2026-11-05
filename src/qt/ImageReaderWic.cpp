#include "ImageReaderInternal.h"
#include "platform/PlatformWic.h"
#include <QColorSpace>
#include <cstring>

namespace StillImages::Internal {

// Windows Imaging Component (WIC) decoder for JPEG XR (.jxr/.wdp/.hdp), DDS, and WIC-supported RAW
QImage decodeWithWic(const QByteArray& bytes, std::atomic<bool>& cancel, HRESULT& error) {
    Platform::WicDecodedImage decoded;
    if (!Platform::DecodeWicImage(reinterpret_cast<const uint8_t*>(bytes.constData()),
                                  static_cast<size_t>(bytes.size()), cancel, error, decoded)) {
        return {};
    }
    QImage img(static_cast<int>(decoded.width), static_cast<int>(decoded.height), QImage::Format_ARGB32);
    if (img.isNull()) { error = E_OUTOFMEMORY; return {}; }
    const size_t rowBytes = static_cast<size_t>(decoded.width) * 4;
    for (uint32_t y = 0; y < decoded.height; ++y) {
        std::memcpy(img.scanLine(static_cast<int>(y)), decoded.bgra.data() + static_cast<size_t>(y) * rowBytes, rowBytes);
    }

    img.setColorSpace(QColorSpace::SRgb);
    QImage normalized = toWhiteSRgb(std::move(img));
    if (decoded.orientation > 1) normalized = orient(std::move(normalized), decoded.orientation);
    error = S_OK;
    return normalized;
}

} // namespace StillImages::Internal
