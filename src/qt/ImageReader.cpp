#include "ImageReaderInternal.h"
#include "HeifReader.h"
#include "FileOperations.h"
#include "platform/Platform.h"
#include "PerceptualHash.h"
#include "VideoScanner.h"
#include <QColorSpace>
#include <QFile>
#include <QFileInfo>
#include <QPainter>
#include <QTransform>
#include <QtEndian>

namespace StillImages::Internal {

int exifOrientation(QByteArray data) {
    if (data.startsWith(QByteArray("Exif\0\0", 6))) data.remove(0, 6);
    if (data.size() < 8 || (data.first(2) != "II" && data.first(2) != "MM")) return -1;
    const bool little = data.first(2) == "II";
    auto word = [&](qsizetype offset) {
        return little ? qFromLittleEndian<quint16>(data.constData() + offset)
                      : qFromBigEndian<quint16>(data.constData() + offset);
    };
    auto dword = [&](qsizetype offset) {
        return little ? qFromLittleEndian<quint32>(data.constData() + offset)
                      : qFromBigEndian<quint32>(data.constData() + offset);
    };
    const quint16 magic = word(2);
    if (magic != 42 && magic != 0x4F52 && magic != 0x0055) return -1;
    const quint64 offset = dword(4);
    if (offset < 8 || offset + 2 > quint64(data.size())) return -1;
    const auto count = word(qsizetype(offset));
    if (offset + 2 + quint64(count) * 12 + 4 > quint64(data.size())) return -1;
    for (int i = 0; i < count; ++i) {
        const auto entry = qsizetype(offset + 2 + quint64(i) * 12);
        if (word(entry) != 0x112) continue;
        if (word(entry + 2) != 3 || dword(entry + 4) != 1) return -1;
        const int value = word(entry + 8);
        return value==0 ? 1 : (value >= 1 && value <= 8) ? value : -1;
    }
    return 1;
}

int containerOrientation(QFile& file, bool webp, std::atomic<bool>& cancel, bool* outAnimated) {
    if (outAnimated) *outAnimated = false;
    if (!file.seek(0)) return -1;
    const auto header = file.read(webp ? 12 : 8);
    if (webp ? (header.size() != 12 || header.first(4) != "RIFF" || header.last(4) != "WEBP")
             : (header != QByteArray::fromHex("89504e470d0a1a0a"))) {
        return -1;
    }
    const qint64 end = webp ? qint64(qFromLittleEndian<quint32>(header.constData() + 4)) + 8 : file.size();
    if (end != file.size()) return -1;
    int orientation = 1;
    bool found = false;
    for (qint64 offset = header.size(); offset < end;) {
        if (cancel.load()) return -3;
        if (end - offset < 8 || !file.seek(offset)) return -1;
        const auto chunk = file.read(8);
        if (chunk.size() != 8) return -1;
        const auto size = webp ? qFromLittleEndian<quint32>(chunk.constData() + 4)
                               : qFromBigEndian<quint32>(chunk.constData());
        const auto type = webp ? chunk.first(4) : chunk.last(4);
        const qint64 next = offset + 8 + qint64(size) + (webp ? (size & 1) : 4);
        if (next > end) return -1;
        if (!webp && type == "IEND") return size==0 ? orientation : -1;
        if (!webp && type == "acTL") {
            if (outAnimated) *outAnimated = true;
            return -2;
        }
        if (webp && (type == "ANIM" || type == "ANMF")) {
            if (outAnimated) *outAnimated = true;
        }
        if (type == (webp ? "EXIF" : "eXIf")) {
            if (found || size > 1024 * 1024) return -1;
            const auto bytes = file.read(size);
            if (bytes.size() != qsizetype(size)) return -1;
            orientation = exifOrientation(bytes);
            if (orientation < 0) return -1;
            found = true;
        }
        offset = next;
    }
    return orientation;
}

QImage orient(QImage image, int orientation) {
    switch (orientation) {
    case 2: return image.mirrored(true, false);
    case 3: return image.transformed(QTransform().rotate(180));
    case 4: return image.mirrored(false, true);
    case 5: return image.transformed(QTransform(0, 1, 1, 0, 0, 0));
    case 6: return image.transformed(QTransform().rotate(90));
    case 7: return image.transformed(QTransform(0, -1, -1, 0, 0, 0));
    case 8: return image.transformed(QTransform().rotate(270));
    default: return image;
    }
}

QImage toWhiteSRgb(QImage image) { return StillImages::flattenTransparency(std::move(image)); }


} // namespace StillImages::Internal

namespace StillImages {

using namespace Internal;

QImage flattenTransparency(QImage image) {
    if (image.isNull()) return {};
    if (image.colorSpace().isValid()) image.convertToColorSpace(QColorSpace::SRgb);
    auto flatten = [&](const QColor& background) {
        QImage flat(image.size(), QImage::Format_RGB32);
        if (flat.isNull()) return flat;
        flat.fill(background);
        { QPainter painter(&flat); painter.drawImage(0, 0, image); }
        flat.setColorSpace(QColorSpace::SRgb);
        return flat;
    };
    QImage onWhite = flatten(Qt::white);
    if (onWhite.isNull() || !image.hasAlphaChannel()) return onWhite;
    // Grey range over a sparse grid: a picture that vanishes on white is nearly uniform there.
    auto range = [](const QImage& flat) {
        int low = 255, high = 0;
        const int stepX = std::max(1, flat.width() / 64), stepY = std::max(1, flat.height() / 64);
        for (int y = 0; y < flat.height(); y += stepY) {
            const auto* row = reinterpret_cast<const QRgb*>(flat.constScanLine(y));
            for (int x = 0; x < flat.width(); x += stepX) { const int grey = qGray(row[x]); low = std::min(low, grey); high = std::max(high, grey); }
        }
        return high - low;
    };
    constexpr int flatRange = 16;
    if (range(onWhite) >= flatRange) return onWhite;
    QImage onBlack = flatten(Qt::black);
    return !onBlack.isNull() && range(onBlack) >= flatRange ? onBlack : onWhite;
}

bool supported(const std::wstring& path) {
    return VideoScanner::IsImageExtension(path);
}

QImage read(const VideoFileInfo& file, std::atomic<bool>& cancel, HRESULT& error, QByteArray* actualFormat, bool allowExtended, ImageExtendedInfo* outExt) {
    error = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    if (cancel.load()) { error = HRESULT_FROM_WIN32(ERROR_CANCELLED); return {}; }
    if (!file.identity.valid || !supported(file.fullPath)) { error = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED); return {}; }
    if (file.identity.size > 256ULL * 1024 * 1024) { error = HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE); return {}; }

    Platform::ReadLock lock(file.fullPath);
    if (!lock.valid()) { error = lock.error(); return {}; }
    if (FileOperations::ReadIdentity(file.fullPath) != file.identity) { error = HRESULT_FROM_WIN32(ERROR_FILE_INVALID); return {}; }

    QFile input(QString::fromStdWString(file.fullPath));
    if (!input.open(QIODevice::ReadOnly)) return {};

    ImageExtendedInfo localExt;
    ImageExtendedInfo* ext = outExt ? outExt : (allowExtended ? &localExt : nullptr);
    if (ext) {
        ext->sourceSize={};
        ext->kind = ImageEvidenceKind::StaticImage;
        ext->frameOrPageCount = 1;
        ext->totalDuration100ns = 0;
        ext->fps = 0.0;
        ext->sampledFrames.clear();
        ext->playbackFrames.clear(); ext->frameDurations.clear();
    }

    const QString extension = QFileInfo(input.fileName()).suffix().toLower();
    QImage image;
    QByteArray format;

    auto finishSuccess = [&](QImage img, const QByteArray& fmt, bool alreadyWhiteSRgb = false) -> QImage {
        if (img.isNull()) return {};
        if (img.width() <= 0 || img.height() <= 0 || img.width() > 16384 || img.height() > 16384 ||
            uint64_t(img.width()) * img.height() > 67108864) {
            error = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            return {};
        }
        QImage normalized = alreadyWhiteSRgb ? std::move(img) : toWhiteSRgb(std::move(img));
        if (normalized.isNull()) return {};
        if (cancel.load()) { error = HRESULT_FROM_WIN32(ERROR_CANCELLED); return {}; }
        if (FileOperations::ReadIdentity(file.fullPath) != file.identity) { error = HRESULT_FROM_WIN32(ERROR_FILE_INVALID); return {}; }
        if (actualFormat) *actualFormat = fmt;
        error = S_OK;
        return normalized;
    };

    // 1. HEIC / HEIF / HIF / AVIF / AVIFS container family
    if (extension == "heic" || extension == "heif" || extension == "hif" || extension == "avif" || extension == "avifs") {
        image = readHeif(input, cancel, error, format, ext);
        if (image.isNull()) return {};
        return finishSuccess(std::move(image), format);
    }

    // Read file bytes once for content-magic verification and decoders
    const QByteArray bytes = input.readAll();
    if (bytes.size() != input.size() || bytes.isEmpty()) return {};

    // Legacy 6-byte "GIF89a" stub check (preserved for ImageChecks legacy contract)
    if (bytes == QByteArray("GIF89a") || bytes == QByteArray("GIF87a")) {
        error = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        return {};
    }

    // 2. SVG / SVGZ vector graphics (strict local rasterization policy)
    if (extension == "svg" || extension == "svgz") {
        image = decodeSvgSafe(bytes, extension == "svgz", cancel, error, ext);
        if (image.isNull()) return {};
        return finishSuccess(std::move(image), extension.toLatin1(), true);
    }

    // 3. Camera RAW formats (.dng, .cr2, .cr3, .nef, .arw, .raf, .orf, .rw2, etc.)
    // 3. Camera RAW formats (.dng, .cr2, .cr3, .nef, .arw, .raf, .orf, .rw2, etc.)
    if (isRawExtension(extension)) {
        bool alreadyWhiteSRgb = false;
        image = decodeCameraRaw(extension, bytes, cancel, error, alreadyWhiteSRgb, ext);
        if (image.isNull()) return {};
        return finishSuccess(std::move(image), extension.toLatin1(), alreadyWhiteSRgb);
    }

    // 4. PSD / PSB composite document
    if (bytes.size() >= 6 && bytes.startsWith("8BPS") &&
        (qFromBigEndian<quint16>(bytes.constData() + 4) == 1 || qFromBigEndian<quint16>(bytes.constData() + 4) == 2)) {
        const QByteArray psdFmt = (qFromBigEndian<quint16>(bytes.constData() + 4) == 2) ? "psb" : "psd";
        image = decodePacketWithFFmpegCodec(AV_CODEC_ID_PSD, bytes, cancel, error);
        if (image.isNull()) image = decodePsdNative(bytes, error);
        if (image.isNull()) return {};
        if (ext) {
            ext->kind = ImageEvidenceKind::CompositeDocument;
            ext->frameOrPageCount = 1;
        }
        return finishSuccess(std::move(image), psdFmt, true);
    }

    // 5. JPEG XL (.jxl)
    if ((bytes.size() >= 2 && static_cast<uint8_t>(bytes[0]) == 0xFF && static_cast<uint8_t>(bytes[1]) == 0x0A) ||
        (bytes.size() >= 12 && bytes.startsWith(QByteArray::fromHex("0000000c4a584c200d0a870a")))) {
        image = decodePacketWithFFmpegCodec(AV_CODEC_ID_JPEGXL, bytes, cancel, error);
        if (image.isNull()) return {};
        return finishSuccess(std::move(image), "jxl", true);
    }

    // 6. JPEG 2000 (.jp2, .j2k, .jpc, .jpf, .jpx, .j2c) & JPEG-LS (.jls)
    if ((bytes.size() >= 12 && bytes.startsWith(QByteArray::fromHex("0000000c6a5020200d0a870a"))) ||
        (bytes.size() >= 4 && bytes.startsWith(QByteArray::fromHex("ff4fff51")))) {
        image = decodePacketWithFFmpegCodec(AV_CODEC_ID_JPEG2000, bytes, cancel, error);
        if (image.isNull()) return {};
        return finishSuccess(std::move(image), "jp2", true);
    }
    if (bytes.size() >= 4 && bytes.startsWith(QByteArray::fromHex("ffd8fff7"))) {
        image = decodePacketWithFFmpegCodec(AV_CODEC_ID_JPEGLS, bytes, cancel, error);
        if (image.isNull()) return {};
        return finishSuccess(std::move(image), "jls", true);
    }

    // 7. JPEG XR / HD Photo (.jxr, .wdp, .hdp)
    if (bytes.size() >= 4 && bytes.startsWith(QByteArray::fromHex("4949bc01"))) {
        image = decodeWithWic(bytes, cancel, error);
        if (image.isNull()) return {};
        return finishSuccess(std::move(image), "jxr", true);
    }

    // 8. OpenEXR (.exr), Radiance HDR (.hdr, .pic), PFM/PHM (.pfm, .phm)
    if (bytes.size() >= 4 && bytes.startsWith(QByteArray::fromHex("762f3101"))) {
        bool isHdr = false;
        image = decodePacketWithFFmpegCodec(AV_CODEC_ID_EXR, bytes, cancel, error, &isHdr);
        if (image.isNull()) return {};
        if (ext) {
            ext->kind = ImageEvidenceKind::TonemappedHdr;
            ext->frameOrPageCount = 1;
        }
        return finishSuccess(std::move(image), "exr", true);
    }
    if (bytes.startsWith("#?RADIANCE") || bytes.startsWith("#?RGBE") || bytes.startsWith("FORMAT=32-bit_rle_rgbe")) {
        image = decodeRadianceHdrNative(bytes, error);
        if (image.isNull()) {
            bool isHdr = false;
            image = decodePacketWithFFmpegCodec(AV_CODEC_ID_RADIANCE_HDR, bytes, cancel, error, &isHdr);
        }
        if (image.isNull()) return {};
        if (ext) {
            ext->kind = ImageEvidenceKind::TonemappedHdr;
            ext->frameOrPageCount = 1;
        }
        return finishSuccess(std::move(image), "hdr", true);
    }
    if (bytes.size() >= 12 && (bytes.startsWith("PF") || bytes.startsWith("Pf") || bytes.startsWith("PH") || bytes.startsWith("Ph")) &&
        (bytes[2] == ' ' || bytes[2] == '\n' || bytes[2] == '\r' || bytes[2] == '\t')) {
        const bool isHalf = (bytes[1] == 'H' || bytes[1] == 'h');
        image = isHalf
            ? decodePacketWithFFmpegCodec(AV_CODEC_ID_PHM, bytes, cancel, error)
            : decodePfmNative(bytes, error);
        if (image.isNull() && !isHalf) image = decodePacketWithFFmpegCodec(AV_CODEC_ID_PFM, bytes, cancel, error);
        if (image.isNull()) return {};
        if (ext) {
            ext->kind = ImageEvidenceKind::TonemappedHdr;
            ext->frameOrPageCount = 1;
        }
        return finishSuccess(std::move(image), isHalf ? "phm" : "pfm", true);
    }

    // 9. PAM (P7), DDS, QOI, SGI, Sun Raster, DPX, IFF/ILBM, PCX
    if (bytes.startsWith("P7\n") || bytes.startsWith("P7\r\n")) {
        image = decodePacketWithFFmpegCodec(AV_CODEC_ID_PAM, bytes, cancel, error);
        if (image.isNull()) return {};
        return finishSuccess(std::move(image), "pam", true);
    }
    if (bytes.size() >= 128 && bytes.startsWith("DDS ")) {
        image = decodePacketWithFFmpegCodec(AV_CODEC_ID_DDS, bytes, cancel, error);
        if (image.isNull()) image = decodeWithWic(bytes, cancel, error);
        if (image.isNull()) return {};
        return finishSuccess(std::move(image), "dds", true);
    }
    if (bytes.size() >= 22 && bytes.startsWith("qoif")) {
        image = decodePacketWithFFmpegCodec(AV_CODEC_ID_QOI, bytes, cancel, error);
        if (image.isNull()) return {};
        return finishSuccess(std::move(image), "qoi", true);
    }
    if (bytes.size() >= 512 && static_cast<uint8_t>(bytes[0]) == 0x01 && static_cast<uint8_t>(bytes[1]) == 0xDA) {
        image = decodePacketWithFFmpegCodec(AV_CODEC_ID_SGI, bytes, cancel, error);
        if (image.isNull()) return {};
        return finishSuccess(std::move(image), "sgi", true);
    }
    if (bytes.size() >= 32 && bytes.startsWith(QByteArray::fromHex("59a66a95"))) {
        image = decodePacketWithFFmpegCodec(AV_CODEC_ID_SUNRAST, bytes, cancel, error);
        if (image.isNull()) return {};
        return finishSuccess(std::move(image), "ras", true);
    }
    if (bytes.size() >= 1024 && (bytes.startsWith("SDPX") || bytes.startsWith("XPDS"))) {
        image = decodePacketWithFFmpegCodec(AV_CODEC_ID_DPX, bytes, cancel, error);
        if (image.isNull()) return {};
        return finishSuccess(std::move(image), "dpx", true);
    }
    if (bytes.size() >= 16 && bytes.startsWith("FORM") &&
        (QByteArray(bytes.constData() + 8, 4) == "ILBM" || QByteArray(bytes.constData() + 8, 4) == "PBM ")) {
        image = decodePacketWithFFmpegCodec(AV_CODEC_ID_IFF_ILBM, bytes, cancel, error);
        if (image.isNull()) return {};
        return finishSuccess(std::move(image), "iff", true);
    }
    if ((extension == "pcx" || bytes.size() >= 128) && static_cast<uint8_t>(bytes[0]) == 0x0A &&
        static_cast<uint8_t>(bytes[1]) <= 5 && static_cast<uint8_t>(bytes[2]) <= 1 &&
        (static_cast<uint8_t>(bytes[3]) == 1 || static_cast<uint8_t>(bytes[3]) == 2 ||
         static_cast<uint8_t>(bytes[3]) == 4 || static_cast<uint8_t>(bytes[3]) == 8)) {
        image = decodePacketWithFFmpegCodec(AV_CODEC_ID_PCX, bytes, cancel, error);
        if (image.isNull()) return {};
        return finishSuccess(std::move(image), "pcx", true);
    }

    // 10. PNG / APNG & WebP EXIF orientation and animation detection
    if (bytes.startsWith(QByteArray::fromHex("89504e470d0a1a0a")) ||
        (bytes.size() >= 12 && bytes.first(4) == "RIFF" && bytes.mid(8, 4) == "WEBP")) {
        const bool isWebp = (bytes.first(4) == "RIFF");
        bool isAnimatedContainer = false;
        input.seek(0);
        const int orientation = containerOrientation(input, isWebp, cancel, &isAnimatedContainer);
        if (orientation == -3) { error = HRESULT_FROM_WIN32(ERROR_CANCELLED); return {}; }
        if (orientation == -1) { error = HRESULT_FROM_WIN32(ERROR_INVALID_DATA); return {}; }
        if (orientation == -2 || isAnimatedContainer) {
            // Animated PNG (APNG) or Animated WebP
            if (!ext) {
                error = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
                return {};
            }
            if (!isWebp) {
                image = decodeApng(bytes, orientation > 0 ? orientation : 1, cancel, error, *ext);
                if (image.isNull()) return {};
                return finishSuccess(std::move(image), "apng", true);
            }
        }
    }

    // 11. Standard & Extended Qt QImageReader formats (JPEG, PNG, WebP, GIF, BMP, TIFF, TGA, ICO, CUR, ICNS, PBM, PGM, PPM, XBM, XPM, WBMP)
    bool alreadyWhiteSRgb = false;
    image = decodeWithQt(input, extension, bytes, cancel, error, format, alreadyWhiteSRgb, ext);
    if (image.isNull()) return {};
    return finishSuccess(std::move(image), format, alreadyWhiteSRgb);
}

QImage readFrameOrPage(const VideoFileInfo& file, int index, std::atomic<bool>& cancel, HRESULT& error, int* outTotalCount) {
    ImageExtendedInfo ext;
    ext.requestedIndex = std::max(0, index);
    QByteArray fmt;
    QImage img = read(file, cancel, error, &fmt, true, &ext);
    if (outTotalCount) *outTotalCount = static_cast<int>(ext.frameOrPageCount);
    return img;
}

bool analyze(VideoFileInfo& file, std::atomic<bool>& cancel) {
    QByteArray format;
    ImageExtendedInfo ext;
    const auto image = read(file, cancel, file.error, &format, true, &ext);
    if (image.isNull()) {
        file.corrupted = !cancel.load();
        return false;
    }
    file.width = ext.sourceSize.isValid() ? ext.sourceSize.width() : image.width();
    file.height = ext.sourceSize.isValid() ? ext.sourceSize.height() : image.height();
    file.imageKind = ext.kind;
    file.imageFrameCount = std::max<uint32_t>(1, ext.frameOrPageCount);
    file.duration100ns = ext.totalDuration100ns;
    file.fps = ext.fps;
    file.audioChannels = 0;
    file.codec = QString::fromLatin1(format).toUpper().toStdWString();

    file.sigFirst = PerceptualHash::ComputeSignatureFromBGRA(image.constBits(), image.width(), image.height(), image.bytesPerLine(),true);
    PerceptualHash::AddNormalizedOrientations(image.constBits(), image.width(), image.height(), image.bytesPerLine(), file.sigFirst);

    file.samples.clear();
    if ((ext.kind == ImageEvidenceKind::AnimatedImage || ext.kind == ImageEvidenceKind::MultiPageDocument) &&
        ext.sampledFrames.size() >= 2) {
        file.samples.reserve(ext.sampledFrames.size());
        for (size_t i = 0; i < ext.sampledFrames.size(); ++i) {
            const auto& sampleImg = ext.sampledFrames[i];
            FrameSignature sig = PerceptualHash::ComputeSignatureFromBGRA(
                sampleImg.constBits(), sampleImg.width(), sampleImg.height(), sampleImg.bytesPerLine(),true);
            sig.timestamp100ns = static_cast<int64_t>(i);
            PerceptualHash::AddNormalizedOrientations(
                sampleImg.constBits(), sampleImg.width(), sampleImg.height(), sampleImg.bytesPerLine(), sig);
            file.samples.push_back(std::move(sig));
        }
        file.sigAlt = file.samples.back();
    } else {
        file.sigAlt = {};
    }

    QImage thumb(THUMB_W, THUMB_H, QImage::Format_ARGB32_Premultiplied);
    thumb.fill(Qt::transparent);
    const auto scaled = image.scaled(THUMB_W, THUMB_H, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    {
        QPainter painter(&thumb);
        painter.drawImage((THUMB_W - scaled.width()) / 2, (THUMB_H - scaled.height()) / 2, scaled);
    }
    file.thumbBGRA.assign(thumb.constBits(), thumb.constBits() + thumb.sizeInBytes());
    file.metadataLoaded = file.frameLoaded = true;
    file.corrupted = false;
    return true;
}

} // namespace StillImages
