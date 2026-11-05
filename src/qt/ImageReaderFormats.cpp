#include "ImageReaderInternal.h"
#include <QBuffer>
#include <QColorSpace>
#include <QImageReader>
#include <QSet>
#include <QtEndian>
#include <cmath>
#include <set>

namespace StillImages::Internal {

// Native Radiance RGBE (.hdr / .pic) fallback parser -> Reinhard tonemap -> sRGB
QImage decodeRadianceHdrNative(const QByteArray& bytes, HRESULT& error) {
    if (bytes.size() < 16 || (!bytes.startsWith("#?RADIANCE") && !bytes.startsWith("#?RGBE") && !bytes.startsWith("FORMAT=32-bit_rle_rgbe"))) {
        return {};
    }
    const char* data = bytes.constData();
    const qsizetype size = bytes.size();
    qsizetype pos = 0;
    // Read header lines until empty line
    while (pos < size) {
        qsizetype lineEnd = pos;
        while (lineEnd < size && data[lineEnd] != '\n') ++lineEnd;
        QByteArray line(data + pos, lineEnd - pos);
        if (line.endsWith('\r')) line.chop(1);
        pos = (lineEnd < size) ? (lineEnd + 1) : size;
        if (line.isEmpty()) break;
    }
    if (pos >= size) return {};
    // Read resolution line e.g. "-Y 128 +X 256"
    qsizetype resEnd = pos;
    while (resEnd < size && data[resEnd] != '\n') ++resEnd;
    QByteArray resLine(data + pos, resEnd - pos);
    if (resLine.endsWith('\r')) resLine.chop(1);
    pos = (resEnd < size) ? (resEnd + 1) : size;

    int width = 0, height = 0;
    if (std::sscanf(resLine.constData(), "-Y %d +X %d", &height, &width) != 2 &&
        std::sscanf(resLine.constData(), "+Y %d +X %d", &height, &width) != 2) {
        return {};
    }
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384 || uint64_t(width) * height > 67108864) return {};

    QImage out(width, height, QImage::Format_RGB32);
    if (out.isNull()) { error = E_OUTOFMEMORY; return {}; }

    auto rgbeToSrgb = [](uint8_t r, uint8_t g, uint8_t b, uint8_t e, uint8_t* dst) {
        if (e == 0) {
            dst[0] = dst[1] = dst[2] = 0;
            dst[3] = 255;
            return;
        }
        const float scale = std::ldexp(1.0f, int(e) - (128 + 8));
        dst[0] = tonemapFloatToSrgbByte((b + 0.5f) * scale);
        dst[1] = tonemapFloatToSrgbByte((g + 0.5f) * scale);
        dst[2] = tonemapFloatToSrgbByte((r + 0.5f) * scale);
        dst[3] = 255;
    };

    std::vector<uint8_t> scanline(size_t(width) * 4);
    for (int y = 0; y < height; ++y) {
        if (pos + 4 > size) return {};
        const uint8_t b0 = static_cast<uint8_t>(data[pos]);
        const uint8_t b1 = static_cast<uint8_t>(data[pos + 1]);
        const uint8_t b2 = static_cast<uint8_t>(data[pos + 2]);
        const uint8_t b3 = static_cast<uint8_t>(data[pos + 3]);

        if (b0 == 2 && b1 == 2 && (b2 & 0x80) == 0 && width >= 8 && width <= 32767) {
            const int lineW = (int(b2) << 8) | int(b3);
            if (lineW != width) return {};
            pos += 4;
            for (int ch = 0; ch < 4; ++ch) {
                int x = 0;
                while (x < width) {
                    if (pos >= size) return {};
                    const uint8_t code = static_cast<uint8_t>(data[pos++]);
                    if (code > 128) {
                        const int run = code - 128;
                        if (x + run > width || pos >= size) return {};
                        const uint8_t val = static_cast<uint8_t>(data[pos++]);
                        for (int k = 0; k < run; ++k) scanline[size_t(x++) * 4 + ch] = val;
                    } else {
                        const int run = code;
                        if (run == 0 || x + run > width || pos + run > size) return {};
                        for (int k = 0; k < run; ++k) scanline[size_t(x++) * 4 + ch] = static_cast<uint8_t>(data[pos++]);
                    }
                }
            }
        } else {
            if (pos + qsizetype(width) * 4 > size) return {};
            memcpy(scanline.data(), data + pos, size_t(width) * 4);
            pos += qsizetype(width) * 4;
        }
        uint8_t* dstRow = out.scanLine(y);
        for (int x = 0; x < width; ++x) {
            rgbeToSrgb(scanline[x * 4 + 0], scanline[x * 4 + 1], scanline[x * 4 + 2], scanline[x * 4 + 3], dstRow + x * 4);
        }
    }
    out.setColorSpace(QColorSpace::SRgb);
    error = S_OK;
    return out;
}

// Native PFM (PF / Pf) float parser -> Reinhard tonemap -> sRGB
QImage decodePfmNative(const QByteArray& bytes, HRESULT& error) {
    if (bytes.size() < 12 || (bytes.first(2) != "PF" && bytes.first(2) != "Pf")) return {};
    const bool color = (bytes[1] == 'F');
    const char* data = bytes.constData();
    const qsizetype size = bytes.size();
    qsizetype pos = 2;
    auto skipWsAndComments = [&]() {
        while (pos < size) {
            if (data[pos] == '#') {
                while (pos < size && data[pos] != '\n') ++pos;
            } else if (data[pos] == ' ' || data[pos] == '\t' || data[pos] == '\r' || data[pos] == '\n') {
                ++pos;
            } else {
                break;
            }
        }
    };
    auto readToken = [&]() -> QByteArray {
        skipWsAndComments();
        qsizetype start = pos;
        while (pos < size && data[pos] != ' ' && data[pos] != '\t' && data[pos] != '\r' && data[pos] != '\n') ++pos;
        return QByteArray(data + start, pos - start);
    };
    bool okW = false, okH = false, okS = false;
    const int width = readToken().toInt(&okW);
    const int height = readToken().toInt(&okH);
    const float scaleToken = readToken().toFloat(&okS);
    if (!okW || !okH || !okS || width <= 0 || height <= 0 || width > 16384 || height > 16384 ||
        uint64_t(width) * height > 67108864 || scaleToken == 0.0f) {
        return {};
    }
    // Single whitespace character after scale header
    if (pos < size && (data[pos] == '\r' && pos + 1 < size && data[pos + 1] == '\n')) pos += 2;
    else if (pos < size) ++pos;

    const int channels = color ? 3 : 1;
    const qsizetype needed = qsizetype(width) * qsizetype(height) * channels * 4;
    if (pos + needed > size) return {};
    const bool little = (scaleToken < 0.0f);
    const float absScale = std::abs(scaleToken);

    QImage out(width, height, QImage::Format_RGB32);
    if (out.isNull()) { error = E_OUTOFMEMORY; return {}; }
    for (int fy = 0; fy < height; ++fy) {
        // PFM stores rows bottom-to-top
        const int dstY = height - 1 - fy;
        uint8_t* dst = out.scanLine(dstY);
        const char* rowPtr = data + pos + qsizetype(fy) * width * channels * 4;
        for (int x = 0; x < width; ++x) {
            auto readFloat = [&](int c) -> float {
                const quint32 raw = little
                    ? qFromLittleEndian<quint32>(rowPtr + (x * channels + c) * 4)
                    : qFromBigEndian<quint32>(rowPtr + (x * channels + c) * 4);
                float val = 0.0f;
                memcpy(&val, &raw, 4);
                return val * absScale;
            };
            if (color) {
                dst[x * 4 + 2] = tonemapFloatToSrgbByte(readFloat(0));
                dst[x * 4 + 1] = tonemapFloatToSrgbByte(readFloat(1));
                dst[x * 4 + 0] = tonemapFloatToSrgbByte(readFloat(2));
                dst[x * 4 + 3] = 255;
            } else {
                const uint8_t v = tonemapFloatToSrgbByte(readFloat(0));
                dst[x * 4 + 0] = dst[x * 4 + 1] = dst[x * 4 + 2] = v;
                dst[x * 4 + 3] = 255;
            }
        }
    }
    out.setColorSpace(QColorSpace::SRgb);
    error = S_OK;
    return out;
}

// Native PSD / PSB merged composite fallback reader (Uncompressed & RLE PackBits, 8/16-bit Grayscale/RGB/CMYK)
QImage decodePsdNative(const QByteArray& bytes, HRESULT& error) {
    if (bytes.size() < 26 || bytes.first(4) != "8BPS") return {};
    const char* d = bytes.constData();
    const qsizetype sz = bytes.size();
    const quint16 version = qFromBigEndian<quint16>(d + 4);
    if (version != 1 && version != 2) return {};
    const quint16 channels = qFromBigEndian<quint16>(d + 12);
    const quint32 height = qFromBigEndian<quint32>(d + 14);
    const quint32 width = qFromBigEndian<quint32>(d + 18);
    const quint16 depth = qFromBigEndian<quint16>(d + 22);
    const quint16 colorMode = qFromBigEndian<quint16>(d + 24);
    if (channels < 1 || channels > 56 || width == 0 || height == 0 ||
        width > 16384 || height > 16384 || uint64_t(width) * height > 67108864 ||
        (depth != 8 && depth != 16) || (colorMode != 1 && colorMode != 3 && colorMode != 4)) {
        return {};
    }
    qsizetype pos = 26;
    if (pos + 4 > sz) return {};
    const quint32 colorDataLen = qFromBigEndian<quint32>(d + pos);
    pos += 4 + qsizetype(colorDataLen);
    if (pos + 4 > sz) return {};
    const quint32 resourcesLen = qFromBigEndian<quint32>(d + pos);
    pos += 4 + qsizetype(resourcesLen);
    if (version == 1) {
        if (pos + 4 > sz) return {};
        const quint32 layerLen = qFromBigEndian<quint32>(d + pos);
        pos += 4 + qsizetype(layerLen);
    } else {
        if (pos + 8 > sz) return {};
        const quint64 layerLen = qFromBigEndian<quint64>(d + pos);
        pos += 8 + qsizetype(layerLen);
    }
    if (pos + 2 > sz) return {};
    const quint16 compression = qFromBigEndian<quint16>(d + pos);
    pos += 2;
    if (compression != 0 && compression != 1) return {};

    const int bytesPerSample = (depth == 16) ? 2 : 1;
    const int neededChannels = (colorMode == 1) ? 1 : (colorMode == 3) ? std::min<int>(channels, 4) : 4;
    if (channels < ((colorMode == 1) ? 1 : (colorMode == 3) ? 3 : 4)) return {};

    const size_t planeSamples = size_t(width) * size_t(height);
    std::vector<uint8_t> planes(size_t(neededChannels) * planeSamples, 255);

    if (compression == 0) {
        const qsizetype planeBytes = qsizetype(planeSamples) * bytesPerSample;
        if (pos + planeBytes * neededChannels > sz) return {};
        for (int ch = 0; ch < neededChannels; ++ch) {
            const uint8_t* src = reinterpret_cast<const uint8_t*>(d + pos + ch * planeBytes);
            uint8_t* dst = planes.data() + size_t(ch) * planeSamples;
            if (bytesPerSample == 1) {
                memcpy(dst, src, planeSamples);
            } else {
                for (size_t i = 0; i < planeSamples; ++i) dst[i] = src[i * 2]; // MSB of big-endian 16-bit
            }
        }
    } else {
        // PackBits RLE: row byte counts for all channels * height rows
        const qsizetype countEntrySize = (version == 2) ? 4 : 2;
        const qsizetype countsBytes = qsizetype(channels) * qsizetype(height) * countEntrySize;
        if (pos + countsBytes > sz) return {};
        pos += countsBytes;
        const size_t rowByteWidth = size_t(width) * bytesPerSample;
        std::vector<uint8_t> rowBuf(rowByteWidth);
        for (int ch = 0; ch < channels; ++ch) {
            for (quint32 y = 0; y < height; ++y) {
                size_t outIdx = 0;
                while (outIdx < rowByteWidth) {
                    if (pos >= sz) return {};
                    const int8_t hdr = static_cast<int8_t>(d[pos++]);
                    if (hdr >= 0) {
                        const size_t count = size_t(hdr) + 1;
                        if (outIdx + count > rowByteWidth || pos + qsizetype(count) > sz) return {};
                        if (ch < neededChannels) memcpy(rowBuf.data() + outIdx, d + pos, count);
                        pos += qsizetype(count);
                        outIdx += count;
                    } else if (hdr != -128) {
                        const size_t count = size_t(1 - int(hdr));
                        if (outIdx + count > rowByteWidth || pos >= sz) return {};
                        const uint8_t val = static_cast<uint8_t>(d[pos++]);
                        if (ch < neededChannels) memset(rowBuf.data() + outIdx, val, count);
                        outIdx += count;
                    }
                }
                if (ch < neededChannels) {
                    uint8_t* dstRow = planes.data() + size_t(ch) * planeSamples + size_t(y) * width;
                    if (bytesPerSample == 1) {
                        memcpy(dstRow, rowBuf.data(), width);
                    } else {
                        for (quint32 x = 0; x < width; ++x) dstRow[x] = rowBuf[x * 2];
                    }
                }
            }
        }
    }

    QImage img(width, height, QImage::Format_RGB32);
    if (img.isNull()) { error = E_OUTOFMEMORY; return {}; }
    for (quint32 y = 0; y < height; ++y) {
        uint8_t* dst = img.scanLine(int(y));
        const size_t rowOff = size_t(y) * width;
        for (quint32 x = 0; x < width; ++x) {
            const size_t idx = rowOff + x;
            uint8_t r = 255, g = 255, b = 255, a = 255;
            if (colorMode == 1) {
                r = g = b = planes[idx];
            } else if (colorMode == 3) {
                r = planes[0 * planeSamples + idx];
                g = planes[1 * planeSamples + idx];
                b = planes[2 * planeSamples + idx];
                if (neededChannels == 4) a = planes[3 * planeSamples + idx];
            } else if (colorMode == 4) {
                const uint8_t c = planes[0 * planeSamples + idx];
                const uint8_t m = planes[1 * planeSamples + idx];
                const uint8_t yv = planes[2 * planeSamples + idx];
                const uint8_t k = planes[3 * planeSamples + idx];
                r = static_cast<uint8_t>((uint32_t(c) * uint32_t(k)) / 255u);
                g = static_cast<uint8_t>((uint32_t(m) * uint32_t(k)) / 255u);
                b = static_cast<uint8_t>((uint32_t(yv) * uint32_t(k)) / 255u);
            }
            dst[x * 4 + 0] = static_cast<uint8_t>((uint32_t(b) * a + 255u * (255u - a)) / 255u);
            dst[x * 4 + 1] = static_cast<uint8_t>((uint32_t(g) * a + 255u * (255u - a)) / 255u);
            dst[x * 4 + 2] = static_cast<uint8_t>((uint32_t(r) * a + 255u * (255u - a)) / 255u);
            dst[x * 4 + 3] = 255;
        }
    }
    img.setColorSpace(QColorSpace::SRgb);
    error = S_OK;
    return img;
}

// Extract largest embedded preview JPEG from Camera RAW (TIFF/EP IFD & SubIFD, Fujifilm RAF, Canon CR3)
QByteArray extractRawEmbeddedJpeg(const QByteArray& bytes, int& outExifOrientation) {
    outExifOrientation = 1;
    const char* d = bytes.constData();
    const qsizetype sz = bytes.size();
    if (sz < 32) return {};

    // 1. Fujifilm RAF ("FUJIFILMCCD-RAW ")
    if (sz >= 92 && bytes.startsWith("FUJIFILMCCD-RAW ")) {
        const quint32 jpgOff = qFromBigEndian<quint32>(d + 84);
        const quint32 jpgLen = qFromBigEndian<quint32>(d + 88);
        if (jpgOff >= 92 && jpgLen > 32 && qsizetype(jpgOff) + qsizetype(jpgLen) <= sz &&
            static_cast<uint8_t>(d[jpgOff]) == 0xFF && static_cast<uint8_t>(d[jpgOff + 1]) == 0xD8) {
            return bytes.mid(jpgOff, jpgLen);
        }
    }

    // 2. Canon CR3 ("ftypcrx ")
    if (sz >= 64 && QByteArray(d + 4, 8) == "ftypcrx ") {
        // Scan for embedded JPEG streams (SOI FF D8 FF ... EOI FF D9) inside CR3 boxes, picking largest valid JPEG
        QByteArray bestJpeg;
        qsizetype pos = 0;
        while (pos + 16 < sz) {
            const qsizetype soi = bytes.indexOf(QByteArray::fromHex("ffd8ff"), pos);
            if (soi < 0) break;
            // Check if preceeded by 4-byte BE size in PRVW/THMB/mdat box
            if (soi >= 4) {
                const quint32 boxLen = qFromBigEndian<quint32>(d + soi - 4);
                if (boxLen > 256 && soi + qsizetype(boxLen) <= sz &&
                    static_cast<uint8_t>(d[soi + boxLen - 2]) == 0xFF &&
                    static_cast<uint8_t>(d[soi + boxLen - 1]) == 0xD9) {
                    if (qsizetype(boxLen) > bestJpeg.size()) bestJpeg = bytes.mid(soi, boxLen);
                    pos = soi + qsizetype(boxLen);
                    continue;
                }
            }
            const qsizetype eoi = bytes.indexOf(QByteArray::fromHex("ffd9"), soi + 4);
            if (eoi > soi) {
                const qsizetype candidateLen = eoi + 2 - soi;
                if (candidateLen > bestJpeg.size() && candidateLen <= 64 * 1024 * 1024) {
                    bestJpeg = bytes.mid(soi, candidateLen);
                }
                pos = eoi + 2;
            } else {
                break;
            }
        }
        return bestJpeg;
    }

    // 3. TIFF/EP based RAW (II/MM header: DNG, CR2, NEF, ARW, ORF, RW2, PEF, SRW, ERF, etc.)
    if ((d[0] == 'I' && d[1] == 'I') || (d[0] == 'M' && d[1] == 'M')) {
        const bool little = (d[0] == 'I');
        auto u16 = [&](qsizetype off) -> quint16 {
            return little ? qFromLittleEndian<quint16>(d + off) : qFromBigEndian<quint16>(d + off);
        };
        auto u32 = [&](qsizetype off) -> quint32 {
            return little ? qFromLittleEndian<quint32>(d + off) : qFromBigEndian<quint32>(d + off);
        };
        const quint16 magic = u16(2);
        if (magic != 42 && magic != 0x4F52 && magic != 0x0055) return {};

        std::vector<quint32> ifdQueue;
        std::set<quint32> visitedIfds;
        const quint32 rootIfd = u32(4);
        if (rootIfd >= 8 && qsizetype(rootIfd) + 2 <= sz) ifdQueue.push_back(rootIfd);

        QByteArray bestJpeg;
        for (size_t qi = 0; qi < ifdQueue.size() && qi < 64; ++qi) {
            const quint32 ifdOff = ifdQueue[qi];
            if (!visitedIfds.insert(ifdOff).second) continue;
            if (ifdOff < 8 || qsizetype(ifdOff) + 2 > sz) continue;
            const quint16 numEntries = u16(ifdOff);
            if (numEntries > 512 || qsizetype(ifdOff) + 2 + qsizetype(numEntries) * 12 + 4 > sz) continue;

            quint32 jpgOffset = 0, jpgLength = 0;
            quint32 stripOffset = 0, stripByteCount = 0;
            quint32 rw2JpgOff = 0, rw2JpgLen = 0;
            quint16 compression = 0;

            for (quint16 i = 0; i < numEntries; ++i) {
                const qsizetype e = qsizetype(ifdOff) + 2 + qsizetype(i) * 12;
                const quint16 tag = u16(e);
                const quint16 type = u16(e + 2);
                const quint32 count = u32(e + 4);
                const quint32 valOrOff = u32(e + 8);
                auto scalarVal = [&]() -> quint32 {
                    if (type == 3 && count == 1) return u16(e + 8);
                    if (type == 4 && count == 1) return valOrOff;
                    return valOrOff;
                };

                if (tag == 0x0112 && qi == 0) { // Orientation in root IFD
                    const int o = int(scalarVal());
                    if (o >= 1 && o <= 8) outExifOrientation = o;
                } else if (tag == 0x0103) { // Compression
                    compression = static_cast<quint16>(scalarVal());
                } else if (tag == 0x0201) { // JPEGInterchangeFormat
                    jpgOffset = scalarVal();
                } else if (tag == 0x0202) { // JPEGInterchangeFormatLength
                    jpgLength = scalarVal();
                } else if (tag == 0x0111 && count == 1) { // StripOffsets
                    stripOffset = scalarVal();
                } else if (tag == 0x0117 && count == 1) { // StripByteCounts
                    stripByteCount = scalarVal();
                } else if (tag == 0x002E && count > 32) { // Panasonic RW2 JpgFromRaw
                    rw2JpgOff = valOrOff;
                    rw2JpgLen = count;
                } else if (tag == 0x014A || tag == 0x8769) { // SubIFDs or ExifIFD
                    if (count == 1) {
                        if (valOrOff >= 8 && qsizetype(valOrOff) + 2 <= sz) ifdQueue.push_back(valOrOff);
                    } else if (count > 1 && count <= 16 && qsizetype(valOrOff) + qsizetype(count) * 4 <= sz) {
                        for (quint32 k = 0; k < count; ++k) {
                            const quint32 sub = u32(qsizetype(valOrOff) + k * 4);
                            if (sub >= 8 && qsizetype(sub) + 2 <= sz) ifdQueue.push_back(sub);
                        }
                    }
                }
            }

            auto tryCandidate = [&](quint32 off, quint32 len) {
                if (off >= 8 && len > 32 && qsizetype(off) + qsizetype(len) <= sz &&
                    static_cast<uint8_t>(d[off]) == 0xFF && static_cast<uint8_t>(d[off + 1]) == 0xD8 &&
                    qsizetype(len) > bestJpeg.size()) {
                    bestJpeg = bytes.mid(off, len);
                }
            };
            tryCandidate(jpgOffset, jpgLength);
            tryCandidate(rw2JpgOff, rw2JpgLen);
            if (compression == 6 || compression == 7) {
                tryCandidate(stripOffset, stripByteCount);
            }

            const quint32 nextIfd = u32(qsizetype(ifdOff) + 2 + qsizetype(numEntries) * 12);
            if (nextIfd >= 8 && qsizetype(nextIfd) + 2 <= sz) ifdQueue.push_back(nextIfd);
        }
        return bestJpeg;
    }

    return {};
}

bool isRawExtension(const QString& ext) {
    static const QSet<QString> kRawExts = {
        "dng", "cr2", "cr3", "nef", "nrw", "arw", "srf", "sr2", "raf", "orf",
        "rw2", "pef", "srw", "erf", "kdc", "dcr", "3fr", "mef", "mos", "iiq", "raw"
    };
    return kRawExts.contains(ext);
}

QImage decodeCameraRaw(const QString& extension, const QByteArray& bytes, std::atomic<bool>& cancel, HRESULT& error, bool& outAlreadyWhiteSRgb, ImageExtendedInfo* ext) {
        int rawOrient = 1;
        const QByteArray embeddedJpeg = extractRawEmbeddedJpeg(bytes, rawOrient);
        if (!embeddedJpeg.isEmpty()) {
            QBuffer buf(const_cast<QByteArray*>(&embeddedJpeg));
            buf.open(QIODevice::ReadOnly);
            QImageReader jReader(&buf, "jpeg");
            jReader.setAutoTransform(true);
            const QSize jSize = jReader.size();
            if (jSize.isValid() && jSize.width() <= 16384 && jSize.height() <= 16384 &&
                uint64_t(jSize.width()) * jSize.height() <= 67108864) {
                QImage jImg = jReader.read();
                if (!jImg.isNull()) {
                    if (jReader.transformation() == QImageIOHandler::TransformationNone && rawOrient > 1) {
                        jImg = orient(std::move(jImg), rawOrient);
                    }
                    if (ext) {
                        ext->kind = ImageEvidenceKind::RawEmbeddedPreview;
                        ext->frameOrPageCount = 1;
                    }
                    outAlreadyWhiteSRgb = false;
                    return jImg;
                }
            }
        }
        // Try developed/uncompressed DNG/TIFF or WIC RAW codec
        if (extension == "dng" || bytes.startsWith("II*\0") || bytes.startsWith("MM\0*")) {
            QBuffer buf(const_cast<QByteArray*>(&bytes));
            buf.open(QIODevice::ReadOnly);
            QImageReader tReader(&buf, "tiff");
            tReader.setAutoTransform(true);
            const QSize tSize = tReader.size();
            if (tSize.isValid() && tSize.width() <= 16384 && tSize.height() <= 16384 &&
                uint64_t(tSize.width()) * tSize.height() <= 67108864) {
                QImage tImg = tReader.read();
                if (!tImg.isNull()) {
                    if (ext) {
                        ext->kind = ImageEvidenceKind::RawDeveloped;
                        ext->frameOrPageCount = 1;
                    }
                    outAlreadyWhiteSRgb = false;
                    return tImg;
                }
            }
        }
        QImage wicRaw = decodeWithWic(bytes, cancel, error);
        if (!wicRaw.isNull()) {
            if (ext) {
                ext->kind = ImageEvidenceKind::RawDeveloped;
                ext->frameOrPageCount = 1;
            }
            outAlreadyWhiteSRgb = true;
            return wicRaw;
        }
        // Distinguish valid RAW header with unsupported compression vs corrupt file
        if (bytes.startsWith("II*\0") || bytes.startsWith("MM\0*") || bytes.startsWith("IIRO") ||
            bytes.startsWith("IIU\0") || bytes.startsWith("FUJIFILMCCD-RAW ") ||
            (bytes.size() >= 12 && QByteArray(bytes.constData() + 4, 8) == "ftypcrx ")) {
            error = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        } else {
            error = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        }
        return {};
}

} // namespace StillImages::Internal
