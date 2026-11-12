#include "ImageReaderInternal.h"
#include "HeifReader.h"
#include "FileOperations.h"
#include "platform/Platform.h"
#include "PerceptualHash.h"
#include "VideoScanner.h"
#include <QByteArrayView>
#include <QColorSpace>
#include <QDate>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QPainter>
#include <QTime>
#include <QTimeZone>
#include <QTransform>
#include <QtEndian>
#include <set>

namespace StillImages::Internal {

namespace {

int64_t parseExifDateTimeString(const char* ptr, qsizetype len) {
    if (!ptr || len < 19) return 0;
    qsizetype start = 0;
    while (start < len && (ptr[start] == ' ' || ptr[start] == '\t')) ++start;
    while (len > start && (ptr[len - 1] == '\0' || ptr[len - 1] == ' ' || ptr[len - 1] == '\r' || ptr[len - 1] == '\n')) --len;
    if (len - start < 19) return 0;
    const char* s = ptr + start;
    auto digit = [&](int idx) -> int {
        const unsigned char c = static_cast<unsigned char>(s[idx]);
        return (c >= '0' && c <= '9') ? int(c - '0') : -1;
    };
    auto num2 = [&](int idx) -> int {
        const int a = digit(idx), b = digit(idx + 1);
        return (a >= 0 && b >= 0) ? (a * 10 + b) : -1;
    };
    auto num4 = [&](int idx) -> int {
        const int hi = num2(idx), lo = num2(idx + 2);
        return (hi >= 0 && lo >= 0) ? (hi * 100 + lo) : -1;
    };
    if ((s[4] != ':' && s[4] != '-') || (s[7] != ':' && s[7] != '-') ||
        (s[10] != ' ' && s[10] != 'T') || s[13] != ':' || s[16] != ':') {
        return 0;
    }
    const int year = num4(0), month = num2(5), day = num2(8);
    const int hour = num2(11), minute = num2(14), second = num2(17);
    if (year < 1970 || year > 2100) return 0;
    const QDate date(year, month, day);
    const QTime time(hour, minute, second);
    if (!date.isValid() || !time.isValid()) return 0;
    const qint64 sec = QDateTime(date, time, QTimeZone::UTC).toSecsSinceEpoch();
    return sec > 0 ? static_cast<int64_t>(sec) : 0;
}

} // namespace

int exifOrientation(QByteArray data) {
    if (data.startsWith(QByteArrayView("Exif\0\0", 6))) data.remove(0, 6);
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
    if (magic != 42 && magic != 0x4F52 && magic != 0x5352 && magic != 0x0055) return -1;
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

int64_t exifCaptureTimeSec(QByteArray data) {
    if (data.startsWith(QByteArrayView("Exif\0\0", 6))) data.remove(0, 6);
    const qsizetype sz = data.size();
    if (sz < 8 || (data.first(2) != "II" && data.first(2) != "MM")) return 0;
    const bool little = data.first(2) == "II";
    const char* d = data.constData();
    auto word = [&](qsizetype offset) -> quint16 {
        return little ? qFromLittleEndian<quint16>(d + offset)
                      : qFromBigEndian<quint16>(d + offset);
    };
    auto dword = [&](qsizetype offset) -> quint32 {
        return little ? qFromLittleEndian<quint32>(d + offset)
                      : qFromBigEndian<quint32>(d + offset);
    };
    const quint16 magic = word(2);
    if (magic != 42 && magic != 0x4F52 && magic != 0x5352 && magic != 0x0055) return 0;
    const quint32 root = dword(4);
    if (root < 8 || uint64_t(root) + 2 > uint64_t(sz)) return 0;

    std::vector<quint32> queue{root};
    std::set<quint32> visited;
    int64_t dateOriginal = 0;
    int64_t dateDigitized = 0;

    for (size_t qi = 0; qi < queue.size() && qi < 32; ++qi) {
        const quint32 ifdOff = queue[qi];
        if (!visited.insert(ifdOff).second) continue;
        if (ifdOff < 8 || uint64_t(ifdOff) + 2 > uint64_t(sz)) continue;
        const quint16 count = word(qsizetype(ifdOff));
        if (count > 512 || uint64_t(ifdOff) + 2 + uint64_t(count) * 12 + 4 > uint64_t(sz)) continue;

        for (quint16 i = 0; i < count; ++i) {
            const qsizetype entry = qsizetype(ifdOff) + 2 + qsizetype(i) * 12;
            const quint16 tag = word(entry);
            const quint16 type = word(entry + 2);
            const quint32 n = dword(entry + 4);
            const quint32 valOrOff = dword(entry + 8);

            if ((tag == 0x9003 || tag == 0x9004) && (type == 2 || type == 7) && n >= 19 && n <= 64) {
                if (uint64_t(valOrOff) + uint64_t(n) <= uint64_t(sz)) {
                    const int64_t parsed = parseExifDateTimeString(d + valOrOff, qsizetype(n));
                    if (parsed > 0) {
                        if (tag == 0x9003) return parsed;
                        if (dateDigitized <= 0) dateDigitized = parsed;
                    }
                }
            } else if (tag == 0x8769 || tag == 0x014A) {
                if (n == 1) {
                    if (valOrOff >= 8 && uint64_t(valOrOff) + 2 <= uint64_t(sz)) queue.push_back(valOrOff);
                } else if (n > 1 && n <= 16 && uint64_t(valOrOff) + uint64_t(n) * 4 <= uint64_t(sz)) {
                    for (quint32 k = 0; k < n; ++k) {
                        const quint32 sub = dword(qsizetype(valOrOff) + qsizetype(k) * 4);
                        if (sub >= 8 && uint64_t(sub) + 2 <= uint64_t(sz)) queue.push_back(sub);
                    }
                }
            }
        }
        const quint32 nextIfd = dword(qsizetype(ifdOff) + 2 + qsizetype(count) * 12);
        if (nextIfd >= 8 && uint64_t(nextIfd) + 2 <= uint64_t(sz)) queue.push_back(nextIfd);
    }
    return dateOriginal > 0 ? dateOriginal : dateDigitized;
}

int64_t jpegExifCaptureTimeSec(const QByteArray& bytes) {
    const qsizetype sz = bytes.size();
    const char* d = bytes.constData();
    if (sz < 16 || static_cast<uint8_t>(d[0]) != 0xFF || static_cast<uint8_t>(d[1]) != 0xD8) return 0;
    qsizetype pos = 2;
    while (pos + 4 <= sz) {
        if (static_cast<uint8_t>(d[pos]) != 0xFF) return 0;
        while (pos < sz && static_cast<uint8_t>(d[pos]) == 0xFF) ++pos;
        if (pos >= sz) return 0;
        const uint8_t marker = static_cast<uint8_t>(d[pos++]);
        if (marker == 0xD9 || marker == 0xDA) break;
        if (marker == 0x00 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD8)) continue;
        if (pos + 2 > sz) return 0;
        const quint16 segLen = qFromBigEndian<quint16>(d + pos);
        if (segLen < 2 || pos + qsizetype(segLen) > sz) return 0;
        if (marker == 0xE1 && segLen > 14 && memcmp(d + pos + 2, "Exif\0\0", 6) == 0) {
            const int64_t ct = exifCaptureTimeSec(QByteArray::fromRawData(d + pos + 8, qsizetype(segLen) - 8));
            if (ct > 0) return ct;
        }
        pos += qsizetype(segLen);
    }
    return 0;
}

int containerOrientation(const QByteArray& bytes, bool webp, std::atomic<bool>& cancel, bool* outAnimated, int64_t* outCaptureTimeSec) {
    if (outAnimated) *outAnimated = false;
    if (outCaptureTimeSec) *outCaptureTimeSec = 0;
    const qsizetype hdrSize = webp ? 12 : 8;
    if (bytes.size() < hdrSize) return -1;
    if (webp ? (bytes.first(4) != "RIFF" || bytes.mid(8, 4) != "WEBP")
             : !bytes.startsWith(QByteArray::fromHex("89504e470d0a1a0a"))) {
        return -1;
    }
    const qint64 end = webp ? qint64(qFromLittleEndian<quint32>(bytes.constData() + 4)) + 8 : qint64(bytes.size());
    if (end != bytes.size()) return -1;
    int orientation = 1;
    bool found = false;
    bool animated = false;
    for (qint64 offset = hdrSize; offset < end;) {
        if (cancel.load()) return -3;
        if (end - offset < 8) return -1;
        const char* chunk = bytes.constData() + offset;
        const quint32 size = webp ? qFromLittleEndian<quint32>(chunk + 4)
                                  : qFromBigEndian<quint32>(chunk);
        const QByteArray type(chunk + (webp ? 0 : 4), 4);
        const qint64 next = offset + 8 + qint64(size) + (webp ? (size & 1) : 4);
        if (next > end) return -1;
        if (!webp && type == "IEND") {
            if (size != 0) return -1;
            return (animated && !found) ? -2 : orientation;
        }
        if (!webp && type == "acTL") {
            animated = true;
            if (outAnimated) *outAnimated = true;
        }
        if (webp && (type == "ANIM" || type == "ANMF")) {
            animated = true;
            if (outAnimated) *outAnimated = true;
        }
        if (type == (webp ? "EXIF" : "eXIf")) {
            if (found || size > 1024 * 1024) return -1;
            const QByteArray exifSlice = QByteArray::fromRawData(chunk + 8, qsizetype(size));
            orientation = exifOrientation(exifSlice);
            if (orientation < 0) return -1;
            if (outCaptureTimeSec) *outCaptureTimeSec = exifCaptureTimeSec(exifSlice);
            found = true;
        }
        offset = next;
    }
    return (!webp && animated && !found) ? -2 : orientation;
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
        ext->sourceSize = {};
        ext->kind = ImageEvidenceKind::StaticImage;
        ext->frameOrPageCount = 1;
        ext->totalDuration100ns = 0;
        ext->captureTimeSec = 0;
        ext->fps = 0.0;
        ext->comparisonTruncated = false;
        ext->previewFrame = {};
        ext->sampledFrames.clear();
        ext->playbackFrames.clear();
        ext->frameDurations.clear();
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
        if (image.isNull()) {
            if (ext && ext->comparisonTruncated && actualFormat && !format.isEmpty()) *actualFormat = format;
            return {};
        }
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
        const DdsHeaderInfo ddsHdr = parseDdsHeader(bytes);
        image = decodePacketWithFFmpegCodec(AV_CODEC_ID_DDS, bytes, cancel, error);
        if (image.isNull()) image = decodeWithWic(bytes, cancel, error);
        bool ddsHdrFloat = false;
        if (image.isNull() && ddsHdr.valid && ddsHdr.floatChannels > 0) {
            image = decodeDdsFloatNative(bytes, ddsHdr, error);
            ddsHdrFloat = !image.isNull();
        }
        if (image.isNull()) {
            if (ddsHdr.valid) {
                if (ext) ext->sourceSize = QSize(int(ddsHdr.width), int(ddsHdr.height));
                if (actualFormat) *actualFormat = "dds";
                error = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
            }
            return {};
        }
        if (ext && ddsHdrFloat) {
            ext->kind = ImageEvidenceKind::TonemappedHdr;
            ext->frameOrPageCount = 1;
        }
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

    // 10. PNG / APNG & WebP EXIF orientation and animation detection (single-pass in-memory)
    int precomputedOrientation = 0;
    if (bytes.startsWith(QByteArray::fromHex("89504e470d0a1a0a")) ||
        (bytes.size() >= 12 && bytes.first(4) == "RIFF" && bytes.mid(8, 4) == "WEBP")) {
        const bool isWebp = (bytes.first(4) == "RIFF");
        bool isAnimatedContainer = false;
        const int orientation = containerOrientation(bytes, isWebp, cancel, &isAnimatedContainer, ext ? &ext->captureTimeSec : nullptr);
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
                if (image.isNull()) {
                    if (ext->comparisonTruncated && actualFormat) *actualFormat = "apng";
                    return {};
                }
                return finishSuccess(std::move(image), "apng", true);
            }
        } else {
            precomputedOrientation = orientation;
        }
    }

    if (ext && ext->captureTimeSec <= 0) {
        if (bytes.size() >= 16 && static_cast<uint8_t>(bytes[0]) == 0xFF && static_cast<uint8_t>(bytes[1]) == 0xD8) {
            ext->captureTimeSec = jpegExifCaptureTimeSec(bytes);
        } else if (bytes.size() >= 8 && (bytes.startsWith(QByteArrayView("II*\0", 4)) || bytes.startsWith(QByteArrayView("MM\0*", 4)))) {
            ext->captureTimeSec = exifCaptureTimeSec(bytes);
        }
    }

    // 11. Standard & Extended Qt QImageReader formats (JPEG, PNG, WebP, GIF, BMP, TIFF, TGA, ICO, CUR, ICNS, PBM, PGM, PPM, XBM, XPM, WBMP)
    bool alreadyWhiteSRgb = false;
    image = decodeWithQt(extension, bytes, cancel, error, format, alreadyWhiteSRgb, ext, precomputedOrientation);
    if (image.isNull()) {
        if (ext && ext->comparisonTruncated && actualFormat && !format.isEmpty()) *actualFormat = format;
        return {};
    }
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
    auto makeThumbnail = [](const QImage& src, std::vector<uint8_t>& outThumb) {
        QImage thumb(THUMB_W, THUMB_H, QImage::Format_ARGB32_Premultiplied);
        thumb.fill(Qt::transparent);
        const auto scaled = src.scaled(THUMB_W, THUMB_H, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        {
            QPainter painter(&thumb);
            painter.drawImage((THUMB_W - scaled.width()) / 2, (THUMB_H - scaled.height()) / 2, scaled);
        }
        outThumb.assign(thumb.constBits(), thumb.constBits() + thumb.sizeInBytes());
    };
    if (image.isNull() || ext.comparisonTruncated) {
        if (!cancel.load()) {
            if (ext.captureTimeSec > 0) file.captureTimeSec = ext.captureTimeSec;
            if (ext.sourceSize.isValid()) {
                file.width = ext.sourceSize.width();
                file.height = ext.sourceSize.height();
                file.imageKind = ext.kind;
                file.imageFrameCount = std::max<uint32_t>(1, ext.frameOrPageCount);
                file.duration100ns = ext.totalDuration100ns;
                file.fps = ext.fps;
                file.audioChannels = 0;
                if (!format.isEmpty()) file.codec = QString::fromLatin1(format).toUpper().toStdWString();
                if (!ext.previewFrame.isNull()) makeThumbnail(ext.previewFrame, file.thumbBGRA);
                file.cachedImageRenderVersion = 0;
                file.cachedDetailsVersion = 2;
                file.metadataLoaded = true;
            }
        }
        file.frameLoaded = false;
        file.sigFirst = {};
        file.sigAlt = {};
        file.samples.clear();
        if (cancel.load()) return false;
        DWORD idErr = 0;
        const auto currentId = FileOperations::ReadIdentity(file.fullPath, &idErr);
        if (!currentId.valid || (file.identity.valid && currentId != file.identity) ||
            file.error == HRESULT_FROM_WIN32(ERROR_FILE_INVALID)) {
            file.contentHash.clear();
            file.corrupted = true;
            file.previewFailed = false;
            if (SUCCEEDED(file.error)) file.error = HRESULT_FROM_WIN32(idErr ? idErr : ERROR_FILE_INVALID);
        } else {
            file.previewFailed = true;
            file.corrupted = false;
            if (SUCCEEDED(file.error)) file.error = HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
        }
        return false;
    }
    file.width = ext.sourceSize.isValid() ? ext.sourceSize.width() : image.width();
    file.height = ext.sourceSize.isValid() ? ext.sourceSize.height() : image.height();
    file.captureTimeSec = ext.captureTimeSec;
    file.imageKind = ext.kind;
    file.imageFrameCount = std::max<uint32_t>(1, ext.frameOrPageCount);
    file.duration100ns = ext.totalDuration100ns;
    file.fps = ext.fps;
    file.audioChannels = 0;
    file.codec = QString::fromLatin1(format).toUpper().toStdWString();

    file.sigFirst = PerceptualHash::ComputeSignatureFromBGRA(image.constBits(), image.width(), image.height(), image.bytesPerLine(), true);
    PerceptualHash::AddNormalizedOrientations(image.constBits(), image.width(), image.height(), image.bytesPerLine(), file.sigFirst);

    file.samples.clear();
    if ((ext.kind == ImageEvidenceKind::AnimatedImage || ext.kind == ImageEvidenceKind::MultiPageDocument) &&
        ext.sampledFrames.size() >= 2) {
        file.samples.reserve(ext.sampledFrames.size());
        for (size_t i = 0; i < ext.sampledFrames.size(); ++i) {
            const auto& sampleImg = ext.sampledFrames[i];
            FrameSignature sig = PerceptualHash::ComputeSignatureFromBGRA(
                sampleImg.constBits(), sampleImg.width(), sampleImg.height(), sampleImg.bytesPerLine(), true);
            sig.timestamp100ns = static_cast<int64_t>(i);
            PerceptualHash::AddNormalizedOrientations(
                sampleImg.constBits(), sampleImg.width(), sampleImg.height(), sampleImg.bytesPerLine(), sig);
            file.samples.push_back(std::move(sig));
        }
        file.sigAlt = file.samples.back();
    } else {
        file.sigAlt = {};
    }

    makeThumbnail(image, file.thumbBGRA);
    file.cachedImageRenderVersion = 0;
    file.cachedDetailsVersion = 2;
    file.metadataLoaded = file.frameLoaded = true;
    file.corrupted = false;
    file.previewFailed = false;
    return true;
}

bool refreshCaptureTime(VideoFileInfo& file, std::atomic<bool>& cancel) {
    if (cancel.load()) return false;
    if (!file.identity.valid || !supported(file.fullPath)) return false;
    if (file.identity.size > 256ULL * 1024 * 1024) return false;

    Platform::ReadLock lock(file.fullPath);
    if (!lock.valid() || FileOperations::ReadIdentity(file.fullPath) != file.identity) {
        file.contentHash.clear();
        file.corrupted = true;
        file.previewFailed = false;
        file.error = !lock.valid() ? lock.error() : HRESULT_FROM_WIN32(ERROR_FILE_INVALID);
        return false;
    }

    QFile input(QString::fromStdWString(file.fullPath));
    if (!input.open(QIODevice::ReadOnly)) return false;

    const QString extension = QFileInfo(input.fileName()).suffix().toLower();
    int64_t cap = 0;

    if (extension == "heic" || extension == "heif" || extension == "hif" || extension == "avif" || extension == "avifs") {
        cap = readHeifCaptureTimeSec(input, cancel);
    } else if (isRawExtension(extension)) {
        const QByteArray bytes = input.readAll();
        if (bytes.size() != input.size()) return false;
        cap = rawCaptureTimeSec(bytes);
    } else {
        const QByteArray hdr = input.peek(16);
        if (hdr.size() >= 2 && static_cast<uint8_t>(hdr[0]) == 0xFF && static_cast<uint8_t>(hdr[1]) == 0xD8) {
            const qint64 sz = input.size();
            qint64 pos = 2;
            while (pos + 4 <= sz && !cancel.load()) {
                if (!input.seek(pos)) break;
                unsigned char b = 0;
                if (input.read(reinterpret_cast<char*>(&b), 1) != 1 || b != 0xFF) break;
                ++pos;
                while (pos < sz) {
                    if (input.read(reinterpret_cast<char*>(&b), 1) != 1) { b = 0xFF; break; }
                    ++pos;
                    if (b != 0xFF) break;
                }
                if (b == 0xFF || b == 0xD9 || b == 0xDA) break;
                if (b == 0x00 || b == 0x01 || (b >= 0xD0 && b <= 0xD8)) continue;
                if (pos + 2 > sz) break;
                unsigned char lenBuf[2]{};
                if (input.read(reinterpret_cast<char*>(lenBuf), 2) != 2) break;
                const quint16 segLen = qFromBigEndian<quint16>(lenBuf);
                if (segLen < 2 || pos + qint64(segLen) > sz) break;
                if (b == 0xE1 && segLen > 14) {
                    char exifHdr[6]{};
                    if (input.read(exifHdr, 6) == 6 && memcmp(exifHdr, "Exif\0\0", 6) == 0) {
                        const QByteArray payload = input.read(qint64(segLen) - 8);
                        if (payload.size() == qsizetype(segLen) - 8) {
                            const int64_t ct = exifCaptureTimeSec(payload);
                            if (ct > 0) { cap = ct; break; }
                        }
                    }
                }
                pos += qint64(segLen);
            }
        } else if (hdr.startsWith(QByteArray::fromHex("89504e470d0a1a0a")) ||
                   (hdr.size() >= 12 && hdr.first(4) == "RIFF" && hdr.mid(8, 4) == "WEBP")) {
            const QByteArray bytes = input.readAll();
            if (bytes.size() != input.size()) return false;
            const bool isWebp = (hdr.first(4) == "RIFF");
            if (containerOrientation(bytes, isWebp, cancel, nullptr, &cap) == -3) return false;
        } else if (hdr.size() >= 8 && (hdr.startsWith(QByteArrayView("II*\0", 4)) || hdr.startsWith(QByteArrayView("MM\0*", 4)))) {
            const QByteArray bytes = input.readAll();
            if (bytes.size() != input.size()) return false;
            cap = exifCaptureTimeSec(bytes);
        }
    }

    if (cancel.load()) return false;
    if (FileOperations::ReadIdentity(file.fullPath) != file.identity) {
        file.contentHash.clear();
        file.corrupted = true;
        file.previewFailed = false;
        file.error = HRESULT_FROM_WIN32(ERROR_FILE_INVALID);
        return false;
    }

    file.captureTimeSec = cap;
    file.cachedDetailsVersion = 2;
    file.metadataLoaded = true;
    return true;
}

} // namespace StillImages
