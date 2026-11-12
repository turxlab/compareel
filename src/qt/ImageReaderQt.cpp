#include "ImageReaderInternal.h"
#include <QBuffer>
#include <QImageReader>
#include <QSet>
#include <map>

namespace StillImages::Internal {

QImage decodeWithQt(const QString& extension, const QByteArray& bytes, std::atomic<bool>& cancel, HRESULT& error, QByteArray& outFormat, bool& outAlreadyWhiteSRgb, ImageExtendedInfo* ext, int precomputedOrientation) {
    QImage image;
    QBuffer buffer(const_cast<QByteArray*>(&bytes));
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    reader.setAutoTransform(true);
    reader.setDecideFormatFromContent(true);
    if (extension == "xbm" && bytes.contains("#define") && bytes.contains("_bits")) {
        reader.setDecideFormatFromContent(false);
        reader.setFormat("xbm");
    } else if (extension == "wbmp" && reader.format().isEmpty()) {
        reader.setDecideFormatFromContent(false);
        reader.setFormat("wbmp");
    }
    const QByteArray format = reader.format().toLower();

    // TGA fallback if Qt plugin didn't recognize RLE/colormap TGA
    if (format.isEmpty() && (extension == "tga" || extension == "icb" || extension == "vda" || extension == "vst") && isValidTgaHeader(bytes)) {
        image = decodePacketWithFFmpegCodec(AV_CODEC_ID_TARGA, bytes, cancel, error);
        if (!image.isNull()) { outFormat = "tga"; outAlreadyWhiteSRgb = true; return image; }
    }

    static const QSet<QByteArray> kAllowedQtFormats = {
        "jpeg", "jpg", "png", "webp", "bmp", "gif", "tiff", "tif",
        "tga", "ico", "cur", "icns", "pbm", "pgm", "ppm", "xbm", "xpm", "wbmp"
    };
    if (!kAllowedQtFormats.contains(format)) {
        if (extension == "tga" || extension == "icb" || extension == "vda" || extension == "vst") {
            if (isValidTgaHeader(bytes)) {
                image = decodePacketWithFFmpegCodec(AV_CODEC_ID_TARGA, bytes, cancel, error);
                if (!image.isNull()) { outFormat = "tga"; outAlreadyWhiteSRgb = true; return image; }
            }
        }
        error = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        return {};
    }

    // Multi-size Icon containers (ICO / CUR / ICNS): select canonical largest/highest-depth entry
    if (format == "ico" || format == "cur" || format == "icns") {
        const int count = std::max(1, reader.imageCount());
        QImage bestIcon;
        uint64_t bestScore = 0;
        int validEntries = 0;
        for (int i = 0; i < count && i < 128; ++i) {
            if (i > 0) reader.jumpToImage(i);
            QImage sub = reader.read();
            if (sub.isNull() || sub.width() <= 0 || sub.height() <= 0 ||
                sub.width() > 16384 || sub.height() > 16384 || uint64_t(sub.width()) * sub.height() > 67108864) {
                continue;
            }
            ++validEntries;
            const uint64_t score = uint64_t(sub.width()) * uint64_t(sub.height()) * 100ULL + uint64_t(sub.depth());
            if (score > bestScore || bestIcon.isNull()) {
                bestScore = score;
                bestIcon = std::move(sub);
            }
        }
        if (bestIcon.isNull()) return {};
        if (ext && validEntries > 1) {
            ext->kind = ImageEvidenceKind::MultiSizeIcon;
            ext->frameOrPageCount = static_cast<uint32_t>(validEntries);
        }
        outFormat = format;
        outAlreadyWhiteSRgb = false;
        return bestIcon;
    }

    const QSize size = reader.size();
    if (size.isValid() && (size.width() > 16384 || size.height() > 16384 || uint64_t(size.width()) * size.height() > 67108864)) {
        // JPEG supports native scaled decoding; keep source dimensions without allocating the full image.
        if ((format=="jpeg" || format=="jpg") && size.width()<=131072 && size.height()<=131072 &&
            uint64_t(size.width())*size.height()<=1073741824 && reader.supportsOption(QImageIOHandler::ScaledSize)) {
            reader.setScaledSize(size.scaled(4096,4096,Qt::KeepAspectRatio));
            if(ext) {
                ext->sourceSize=size;
                if(reader.transformation() & QImageIOHandler::TransformationRotate90) ext->sourceSize.transpose();
            }
        } else {
            error=HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE); return {};
        }
    }

    if (format == "tiff" || format == "tif") buffer.seek(0);
    const int imgCount = reader.imageCount();
    if (format == "tiff" || format == "tif") buffer.seek(0);
    const bool isAnimated = (format == "gif" || format == "webp") && (imgCount > 1 || (reader.supportsAnimation() && imgCount != 1));
    const bool isMultiPage = (format == "tiff" || format == "tif") && (imgCount > 1);


    if (isAnimated) {
        image = decodeQtAnimated(reader, cancel, error, ext);
        if (!format.isEmpty()) outFormat = format;
        if (image.isNull()) return {};
        outAlreadyWhiteSRgb = true;
        return image;
    }

    if (isMultiPage) {
        if (!ext) {
            error = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
            return {};
        }
        if (imgCount > 256) {
            error = HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
            return {};
        }
        ext->kind = ImageEvidenceKind::MultiPageDocument;
        ext->frameOrPageCount = static_cast<uint32_t>(imgCount);
        if (ext->requestedIndex >= 0) {
            const int target = std::clamp(ext->requestedIndex, 0, imgCount - 1);
            reader.jumpToImage(target);
            buffer.seek(0);
            QImage page = reader.read();
            if (page.isNull()) return {};
            outFormat = "tiff";
            outAlreadyWhiteSRgb = false;
            return page;
        }
        std::map<int, QImage> pageCache;
        ext->sampledFrames.clear();
        for (int k = 0; k < 5; ++k) {
            const int pIdx = (k * (imgCount - 1)) / 4;
            auto it = pageCache.find(pIdx);
            if (it == pageCache.end()) {
                reader.jumpToImage(pIdx);
                buffer.seek(0);
                QImage page = toWhiteSRgb(reader.read());
                if (page.isNull()) return {};
                it = pageCache.emplace(pIdx, std::move(page)).first;
            }
            ext->sampledFrames.push_back(it->second);
        }
        outFormat = "tiff";
        outAlreadyWhiteSRgb = true;
        return ext->sampledFrames[0];
    }

    int orientation = (precomputedOrientation > 0) ? precomputedOrientation : 1;
    if (precomputedOrientation == 0 && (format == "webp" || format == "png")) {
        orientation = containerOrientation(bytes, format == "webp", cancel);
        if (orientation < 0) {
            if (orientation == -2) error = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
            if (orientation == -3) error = HRESULT_FROM_WIN32(ERROR_CANCELLED);
            return {};
        }
    }

    image = reader.read();
    if (image.isNull() && format == "tga") {
        image = decodePacketWithFFmpegCodec(AV_CODEC_ID_TARGA, bytes, cancel, error);
        if (!image.isNull()) { outFormat = "tga"; outAlreadyWhiteSRgb = true; return image; }
    }
    if (image.isNull()) return {};
    if ((format == "webp" || format == "png") && reader.transformation() == QImageIOHandler::TransformationNone && orientation > 1) {
        image = orient(std::move(image), orientation);
    }
    outFormat = format;
    outAlreadyWhiteSRgb = false;
    return image;
}

} // namespace StillImages::Internal
