#include "ImageReaderInternal.h"
#include <QBuffer>
#include <QByteArrayView>
#include <QColorSpace>
#include <QImageReader>
#include <QSet>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <set>

namespace StillImages::Internal {

namespace {

float halfToFloat(uint16_t h) {
    const uint32_t sign = (h >> 15) & 1u;
    const uint32_t exp = (h >> 10) & 0x1Fu;
    const uint32_t mant = h & 0x3FFu;
    if (exp == 0) {
        const float v = std::ldexp(static_cast<float>(mant), -24);
        return sign ? -v : v;
    }
    if (exp == 31) {
        if (mant == 0 && !sign) return 65504.0f;
        return 0.0f;
    }
    const float v = std::ldexp(static_cast<float>(mant + 1024u), static_cast<int>(exp) - 25);
    return sign ? -v : v;
}

struct JpegStreamInfo {
    bool valid = false;
    int width = 0;
    int height = 0;
    qsizetype byteLength = 0;
};

// Segment-aware JPEG validator: skips APPn EXIF thumbnails cleanly, rejects lossless/RAW sensor SOF3
// or >8-bit streams that QImageReader cannot decode, and finds the exact outer EOI when scanning streams.
JpegStreamInfo inspectJpegStream(const char* base, qsizetype avail, bool scanEntropyToEoi) {
    JpegStreamInfo out;
    if (avail < 32 || static_cast<uint8_t>(base[0]) != 0xFF || static_cast<uint8_t>(base[1]) != 0xD8) {
        return out;
    }
    qsizetype pos = 2;
    int sofW = 0, sofH = 0;
    bool seenSof = false;
    bool seenSos = false;

    while (pos < avail) {
        if (static_cast<uint8_t>(base[pos]) != 0xFF) return out;
        while (pos < avail && static_cast<uint8_t>(base[pos]) == 0xFF) ++pos;
        if (pos >= avail) return out;
        const uint8_t marker = static_cast<uint8_t>(base[pos++]);
        if (marker == 0x00) return out;
        if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            continue;
        }
        if (marker == 0xD9) { // EOI
            if (seenSof && seenSos) {
                out.valid = true;
                out.width = sofW;
                out.height = sofH;
                out.byteLength = pos;
            }
            return out;
        }
        // Reject lossless JPEG (SOF3=0xC3, SOF7=0xC7, SOF11=0xCB, SOF15=0xCF), differential DCT, and JPEG-LS (0xF7)
        if (marker == 0xC3 || marker == 0xC5 || marker == 0xC6 || marker == 0xC7 ||
            marker == 0xCB || marker == 0xCD || marker == 0xCE || marker == 0xCF || marker == 0xF7) {
            return out;
        }
        if (pos + 2 > avail) return out;
        const quint16 segLen = qFromBigEndian<quint16>(base + pos);
        if (segLen < 2 || pos + qsizetype(segLen) > avail) return out;

        if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2 || marker == 0xC9 || marker == 0xCA) {
            if (segLen < 8) return out;
            const uint8_t precision = static_cast<uint8_t>(base[pos + 2]);
            const quint16 h = qFromBigEndian<quint16>(base + pos + 3);
            const quint16 w = qFromBigEndian<quint16>(base + pos + 5);
            const uint8_t comps = static_cast<uint8_t>(base[pos + 7]);
            if (precision != 8 || w == 0 || h == 0 || (comps != 1 && comps != 3 && comps != 4)) {
                return out;
            }
            sofW = int(w);
            sofH = int(h);
            seenSof = true;
        }
        pos += qsizetype(segLen);

        if (marker == 0xDA) { // SOS
            if (!seenSof) return out;
            seenSos = true;
            if (!scanEntropyToEoi) {
                // Known slice length from IFD/box: verify trailing bytes exist and accept
                if (pos >= avail) return out;
                out.valid = true;
                out.width = sofW;
                out.height = sofH;
                out.byteLength = avail;
                return out;
            }
            // Scan entropy-coded bytes past stuffed 0xFF 0x00 and RST0..RST7 until next unstuffed marker
            while (pos < avail) {
                if (static_cast<uint8_t>(base[pos]) != 0xFF) {
                    ++pos;
                    continue;
                }
                while (pos < avail && static_cast<uint8_t>(base[pos]) == 0xFF) ++pos;
                if (pos >= avail) return out;
                const uint8_t nextMarker = static_cast<uint8_t>(base[pos]);
                if (nextMarker == 0x00 || (nextMarker >= 0xD0 && nextMarker <= 0xD7)) {
                    ++pos;
                    continue;
                }
                --pos; // Rewind onto the 0xFF byte so the outer marker loop handles EOI or progressive scans
                break;
            }
        }
    }
    return out;
}

struct RawJpegCandidate {
    qsizetype offset = 0;
    qsizetype length = 0;
    int width = 0;
    int height = 0;
};

struct RawContainerInfo {
    int exifOrientation = 1;
    int64_t captureTimeSec = 0;
    bool ifd0ReducedResolution = false;
    uint32_t maxIfdWidth = 0;
    uint32_t maxIfdHeight = 0;
    std::vector<RawJpegCandidate> jpegs;
};

void parseTiffRootIfd(const char* d, qsizetype sz, RawContainerInfo& out) {
    if (sz < 16 || !((d[0] == 'I' && d[1] == 'I') || (d[0] == 'M' && d[1] == 'M'))) return;
    const bool little = (d[0] == 'I');
    auto u16 = [&](qsizetype off) -> quint16 {
        return little ? qFromLittleEndian<quint16>(d + off) : qFromBigEndian<quint16>(d + off);
    };
    auto u32 = [&](qsizetype off) -> quint32 {
        return little ? qFromLittleEndian<quint32>(d + off) : qFromBigEndian<quint32>(d + off);
    };
    if (u16(2) != 42) return;
    const quint32 root = u32(4);
    if (root < 8 || qsizetype(root) + 2 > sz) return;
    const quint16 count = u16(qsizetype(root));
    if (count > 512 || qsizetype(root) + 2 + qsizetype(count) * 12 > sz) return;
    quint32 ifdW = 0, ifdH = 0;
    for (quint16 i = 0; i < count; ++i) {
        const qsizetype e = qsizetype(root) + 2 + qsizetype(i) * 12;
        const quint16 tag = u16(e);
        const quint16 type = u16(e + 2);
        const quint32 cnt = u32(e + 4);
        if (cnt != 1) continue;
        const quint32 val = (type == 3) ? quint32(u16(e + 8)) : u32(e + 8);
        if (tag == 0x0100 && (type == 3 || type == 4)) {
            ifdW = val;
        } else if (tag == 0x0101 && (type == 3 || type == 4)) {
            ifdH = val;
        } else if (tag == 0x0112 && type == 3 && val >= 1 && val <= 8) {
            out.exifOrientation = int(val);
        }
    }
    if (ifdW > 0 && ifdH > 0 && ifdW <= 131072 && ifdH <= 131072 &&
        uint64_t(ifdW) * ifdH <= 1073741824ULL &&
        uint64_t(ifdW) * ifdH > uint64_t(out.maxIfdWidth) * out.maxIfdHeight) {
        out.maxIfdWidth = ifdW;
        out.maxIfdHeight = ifdH;
    }
}

void scanCr3IsoBoxes(const char* d, qsizetype boxStart, qsizetype boxEnd, int depth, RawContainerInfo& out) {
    if (depth > 6) return;
    qsizetype pos = boxStart;
    while (pos + 8 <= boxEnd) {
        quint64 boxSize = qFromBigEndian<quint32>(d + pos);
        const QByteArray boxType(d + pos + 4, 4);
        qsizetype hdrLen = 8;
        if (boxSize == 1) {
            if (pos + 16 > boxEnd) break;
            boxSize = qFromBigEndian<quint64>(d + pos + 8);
            hdrLen = 16;
        } else if (boxSize == 0) {
            boxSize = quint64(boxEnd - pos);
        }
        if (boxSize < quint64(hdrLen) || boxSize > quint64(boxEnd - pos)) break;
        const qsizetype payloadStart = pos + hdrLen;
        const qsizetype payloadEnd = pos + qsizetype(boxSize);

        if (boxType == "moov") {
            scanCr3IsoBoxes(d, payloadStart, payloadEnd, depth + 1, out);
        } else if (boxType == "uuid" && payloadEnd - payloadStart >= 16) {
            static const QByteArray kCanonCmtUuid = QByteArray::fromHex("85c0b687820f11e08111f4ce462b6a48");
            static const QByteArray kCanonPrvwUuid = QByteArray::fromHex("eaf42b5e1c984b88b9fbb7dc406e4d16");
            const QByteArray uuidBytes(d + payloadStart, 16);
            if (uuidBytes == kCanonCmtUuid || uuidBytes == kCanonPrvwUuid) {
                scanCr3IsoBoxes(d, payloadStart + 16, payloadEnd, depth + 1, out);
            }
        } else if (boxType == "CMT1" && payloadEnd > payloadStart) {
            parseTiffRootIfd(d + payloadStart, payloadEnd - payloadStart, out);
            if (out.captureTimeSec <= 0) {
                out.captureTimeSec = exifCaptureTimeSec(QByteArray::fromRawData(d + payloadStart, payloadEnd - payloadStart));
            }
        } else if (boxType == "CMT2" && payloadEnd > payloadStart) {
            const int64_t ct = exifCaptureTimeSec(QByteArray::fromRawData(d + payloadStart, payloadEnd - payloadStart));
            if (ct > 0) out.captureTimeSec = ct;
        } else if (boxType == "PRVW" && payloadEnd - payloadStart > 16) {
            // Canon PRVW: 8 bytes reserved/dim + 4-byte jpeg size at payload+12, JPEG at payload+16 (or +20 in v1 FullBox)
            for (qsizetype off : {qsizetype(16), qsizetype(20), qsizetype(8), qsizetype(12), qsizetype(0)}) {
                const qsizetype jStart = payloadStart + off;
                if (jStart + 32 <= payloadEnd &&
                    static_cast<uint8_t>(d[jStart]) == 0xFF && static_cast<uint8_t>(d[jStart + 1]) == 0xD8) {
                    const auto jInfo = inspectJpegStream(d + jStart, payloadEnd - jStart, false);
                    if (jInfo.valid) {
                        out.jpegs.push_back({jStart, payloadEnd - jStart, jInfo.width, jInfo.height});
                        break;
                    }
                }
            }
        } else if (boxType == "THMB" && payloadEnd - payloadStart > 16) {
            for (qsizetype off : {qsizetype(16), qsizetype(20), qsizetype(12), qsizetype(8), qsizetype(0)}) {
                const qsizetype jStart = payloadStart + off;
                if (jStart + 32 <= payloadEnd &&
                    static_cast<uint8_t>(d[jStart]) == 0xFF && static_cast<uint8_t>(d[jStart + 1]) == 0xD8) {
                    const auto jInfo = inspectJpegStream(d + jStart, payloadEnd - jStart, false);
                    if (jInfo.valid) {
                        out.jpegs.push_back({jStart, payloadEnd - jStart, jInfo.width, jInfo.height});
                        break;
                    }
                }
            }
        }
        pos = payloadEnd;
    }
}

RawContainerInfo inspectRawContainer(const QByteArray& bytes) {
    RawContainerInfo out;
    const char* d = bytes.constData();
    const qsizetype sz = bytes.size();
    if (sz < 32) return out;

    auto addVerifiedCandidate = [&](qsizetype off, qsizetype len, bool scanToEoi) -> qsizetype {
        if (off < 0 || len <= 32 || off + len > sz) return 0;
        if (static_cast<uint8_t>(d[off]) != 0xFF || static_cast<uint8_t>(d[off + 1]) != 0xD8) return 0;
        const auto info = inspectJpegStream(d + off, len, scanToEoi);
        if (!info.valid) return 0;
        for (const auto& existing : out.jpegs) {
            if (existing.offset == off) return info.byteLength;
        }
        out.jpegs.push_back({off, info.byteLength, info.width, info.height});
        return info.byteLength;
    };

    // 1. Fujifilm RAF ("FUJIFILMCCD-RAW ")
    if (sz >= 92 && bytes.startsWith("FUJIFILMCCD-RAW ")) {
        const quint32 jpgOff = qFromBigEndian<quint32>(d + 84);
        const quint32 jpgLen = qFromBigEndian<quint32>(d + 88);
        if (jpgOff >= 92 && jpgLen > 32 && qsizetype(jpgOff) + qsizetype(jpgLen) <= sz) {
            addVerifiedCandidate(qsizetype(jpgOff), qsizetype(jpgLen), false);
            out.captureTimeSec = jpegExifCaptureTimeSec(QByteArray::fromRawData(d + jpgOff, qsizetype(jpgLen)));
        }
        return out;
    }

    // 2. Canon CR3 ("ftypcrx ")
    if (sz >= 64 && QByteArray(d + 4, 8) == "ftypcrx ") {
        scanCr3IsoBoxes(d, 0, sz, 0, out);
        // Also scan mdat/stream for full-size embedded JPEG using segment-aware EOI detection
        qsizetype pos = 0;
        int attempts = 0;
        while (pos + 32 < sz && attempts < 64) {
            const qsizetype soi = bytes.indexOf(QByteArray::fromHex("ffd8ff"), pos);
            if (soi < 0) break;
            ++attempts;
            if (soi >= 4) {
                const quint32 boxLen = qFromBigEndian<quint32>(d + soi - 4);
                if (boxLen > 256 && boxLen <= 64u * 1024u * 1024u && soi + qsizetype(boxLen) <= sz &&
                    static_cast<uint8_t>(d[soi + boxLen - 2]) == 0xFF &&
                    static_cast<uint8_t>(d[soi + boxLen - 1]) == 0xD9) {
                    if (addVerifiedCandidate(soi, qsizetype(boxLen), false) > 0) {
                        pos = soi + qsizetype(boxLen);
                        continue;
                    }
                }
            }
            const qsizetype maxAvail = std::min<qsizetype>(sz - soi, 64 * 1024 * 1024);
            const qsizetype parsedLen = addVerifiedCandidate(soi, maxAvail, true);
            pos = (parsedLen > 0) ? (soi + parsedLen) : (soi + 3);
        }
    } else if ((d[0] == 'I' && d[1] == 'I') || (d[0] == 'M' && d[1] == 'M')) {
        // 3. TIFF/EP & DNG (II/MM header: DNG, CR2, NEF, ARW, ORF, RW2, PEF, SRW, ERF, etc.)
        const bool little = (d[0] == 'I');
        auto u16 = [&](qsizetype off) -> quint16 {
            return little ? qFromLittleEndian<quint16>(d + off) : qFromBigEndian<quint16>(d + off);
        };
        auto u32 = [&](qsizetype off) -> quint32 {
            return little ? qFromLittleEndian<quint32>(d + off) : qFromBigEndian<quint32>(d + off);
        };
        const quint16 magic = u16(2);
        if (magic != 42 && magic != 0x4F52 && magic != 0x5352 && magic != 0x0055) return out;
        out.captureTimeSec = exifCaptureTimeSec(bytes);

        std::vector<quint32> ifdQueue;
        std::set<quint32> visitedIfds;
        const quint32 rootIfd = u32(4);
        if (rootIfd >= 8 && qsizetype(rootIfd) + 2 <= sz) ifdQueue.push_back(rootIfd);

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
            quint32 subfileType = 0;
            quint32 ifdW = 0, ifdH = 0;

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

                if (tag == 0x00FE && count == 1) { // NewSubfileType
                    subfileType = scalarVal();
                } else if (tag == 0x0100 && count == 1) { // ImageWidth
                    ifdW = scalarVal();
                } else if (tag == 0x0101 && count == 1) { // ImageLength
                    ifdH = scalarVal();
                } else if (tag == 0x0112 && qi == 0) { // Orientation in root IFD
                    const int o = int(scalarVal());
                    if (o >= 1 && o <= 8) out.exifOrientation = o;
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

            if (qi == 0 && (subfileType & 1u) != 0) {
                out.ifd0ReducedResolution = true;
            }
            if (ifdW > 0 && ifdH > 0 && ifdW <= 131072 && ifdH <= 131072 &&
                uint64_t(ifdW) * ifdH <= 1073741824ULL &&
                uint64_t(ifdW) * ifdH > uint64_t(out.maxIfdWidth) * out.maxIfdHeight) {
                out.maxIfdWidth = ifdW;
                out.maxIfdHeight = ifdH;
            }

            if (jpgOffset >= 8 && jpgLength > 32) addVerifiedCandidate(qsizetype(jpgOffset), qsizetype(jpgLength), false);
            if (rw2JpgOff >= 8 && rw2JpgLen > 32) addVerifiedCandidate(qsizetype(rw2JpgOff), qsizetype(rw2JpgLen), false);
            if ((compression == 6 || compression == 7) && stripOffset >= 8 && stripByteCount > 32) {
                addVerifiedCandidate(qsizetype(stripOffset), qsizetype(stripByteCount), false);
            }

            const quint32 nextIfd = u32(qsizetype(ifdOff) + 2 + qsizetype(numEntries) * 12);
            if (nextIfd >= 8 && qsizetype(nextIfd) + 2 <= sz) ifdQueue.push_back(nextIfd);
        }
    }

    std::stable_sort(out.jpegs.begin(), out.jpegs.end(), [](const RawJpegCandidate& a, const RawJpegCandidate& b) {
        const uint64_t areaA = uint64_t(a.width) * uint64_t(a.height);
        const uint64_t areaB = uint64_t(b.width) * uint64_t(b.height);
        if (areaA != areaB) return areaA > areaB;
        return a.length > b.length;
    });
    if (out.captureTimeSec <= 0) {
        for (const auto& jpg : out.jpegs) {
            out.captureTimeSec = jpegExifCaptureTimeSec(QByteArray::fromRawData(d + jpg.offset, jpg.length));
            if (out.captureTimeSec > 0) break;
        }
    }
    return out;
}

} // namespace

int64_t rawCaptureTimeSec(const QByteArray& bytes) {
    return inspectRawContainer(bytes).captureTimeSec;
}

DdsHeaderInfo parseDdsHeader(const QByteArray& bytes) {
    DdsHeaderInfo info;
    if (bytes.size() < 128 || !bytes.startsWith("DDS ")) return info;
    const char* d = bytes.constData();
    const quint32 dwSize = qFromLittleEndian<quint32>(d + 4);
    const quint32 height = qFromLittleEndian<quint32>(d + 12);
    const quint32 width = qFromLittleEndian<quint32>(d + 16);
    const quint32 pfSize = qFromLittleEndian<quint32>(d + 76);
    const quint32 pfFlags = qFromLittleEndian<quint32>(d + 80);
    const quint32 fourCC = qFromLittleEndian<quint32>(d + 84);
    if (dwSize != 124 || pfSize != 32 || width == 0 || height == 0 ||
        width > 131072 || height > 131072 || uint64_t(width) * height > 1073741824ULL) {
        return info;
    }

    qsizetype dataOffset = 128;
    int floatChannels = 0;
    bool isHalfFloat = false;

    constexpr quint32 kDdpfFourCC = 0x4u;
    if ((pfFlags & kDdpfFourCC) != 0) {
        if (QByteArray(d + 84, 4) == "DX10") {
            if (bytes.size() < 148) return info;
            const quint32 dxgiFormat = qFromLittleEndian<quint32>(d + 128);
            const quint32 resourceDim = qFromLittleEndian<quint32>(d + 132);
            if (resourceDim < 2 || resourceDim > 4) return info;
            dataOffset = 148;
            switch (dxgiFormat) {
            case 2:  floatChannels = 4; isHalfFloat = false; break; // DXGI_FORMAT_R32G32B32A32_FLOAT
            case 6:  floatChannels = 3; isHalfFloat = false; break; // DXGI_FORMAT_R32G32B32_FLOAT
            case 10: floatChannels = 4; isHalfFloat = true;  break; // DXGI_FORMAT_R16G16B16A16_FLOAT
            case 16: floatChannels = 2; isHalfFloat = false; break; // DXGI_FORMAT_R32G32_FLOAT
            case 34: floatChannels = 2; isHalfFloat = true;  break; // DXGI_FORMAT_R16G16_FLOAT
            case 41: floatChannels = 1; isHalfFloat = false; break; // DXGI_FORMAT_R32_FLOAT
            case 54: floatChannels = 1; isHalfFloat = true;  break; // DXGI_FORMAT_R16_FLOAT
            default: break;
            }
        } else {
            switch (fourCC) {
            case 111: floatChannels = 1; isHalfFloat = true;  break; // D3DFMT_R16F
            case 112: floatChannels = 2; isHalfFloat = true;  break; // D3DFMT_G16R16F
            case 113: floatChannels = 4; isHalfFloat = true;  break; // D3DFMT_A16B16G16R16F
            case 114: floatChannels = 1; isHalfFloat = false; break; // D3DFMT_R32F
            case 115: floatChannels = 2; isHalfFloat = false; break; // D3DFMT_G32R32F
            case 116: floatChannels = 4; isHalfFloat = false; break; // D3DFMT_A32B32G32R32F
            default: break;
            }
        }
    }

    info.valid = true;
    info.width = width;
    info.height = height;
    info.dataOffset = dataOffset;
    info.floatChannels = floatChannels;
    info.isHalfFloat = isHalfFloat;
    return info;
}

QImage decodeDdsFloatNative(const QByteArray& bytes, const DdsHeaderInfo& hdr, HRESULT& error) {
    if (!hdr.valid || hdr.floatChannels < 1 || hdr.floatChannels > 4 ||
        hdr.width == 0 || hdr.height == 0 || hdr.width > 16384 || hdr.height > 16384 ||
        uint64_t(hdr.width) * hdr.height > 67108864ULL) {
        return {};
    }
    const uint64_t sampleBytes = hdr.isHalfFloat ? 2ULL : 4ULL;
    const uint64_t pixelBytes = uint64_t(hdr.floatChannels) * sampleBytes;
    const uint64_t totalBytes = uint64_t(hdr.width) * uint64_t(hdr.height) * pixelBytes;
    if (hdr.dataOffset < 128 || uint64_t(hdr.dataOffset) + totalBytes > uint64_t(bytes.size())) {
        return {};
    }

    QImage out(int(hdr.width), int(hdr.height), QImage::Format_RGB32);
    if (out.isNull()) { error = E_OUTOFMEMORY; return {}; }

    const char* srcBase = bytes.constData() + hdr.dataOffset;
    for (uint32_t y = 0; y < hdr.height; ++y) {
        const char* rowPtr = srcBase + uint64_t(y) * hdr.width * pixelBytes;
        uint8_t* dst = out.scanLine(int(y));
        for (uint32_t x = 0; x < hdr.width; ++x) {
            const char* px = rowPtr + uint64_t(x) * pixelBytes;
            auto readSample = [&](int ch) -> float {
                if (hdr.isHalfFloat) {
                    return halfToFloat(qFromLittleEndian<quint16>(px + ch * 2));
                }
                const quint32 raw = qFromLittleEndian<quint32>(px + ch * 4);
                float val = 0.0f;
                memcpy(&val, &raw, 4);
                return val;
            };
            uint8_t r8 = 0, g8 = 0, b8 = 0;
            float a = 1.0f;
            if (hdr.floatChannels == 1) {
                r8 = g8 = b8 = tonemapFloatToSrgbByte(readSample(0));
            } else if (hdr.floatChannels == 2) {
                r8 = tonemapFloatToSrgbByte(readSample(0));
                g8 = tonemapFloatToSrgbByte(readSample(1));
                b8 = 0;
            } else {
                r8 = tonemapFloatToSrgbByte(readSample(0));
                g8 = tonemapFloatToSrgbByte(readSample(1));
                b8 = tonemapFloatToSrgbByte(readSample(2));
                if (hdr.floatChannels == 4) {
                    const float rawA = readSample(3);
                    a = std::isfinite(rawA) ? std::clamp(rawA, 0.0f, 1.0f) : 1.0f;
                }
            }
            dst[x * 4 + 0] = static_cast<uint8_t>(std::clamp(int(std::round(b8 * a + 255.0f * (1.0f - a))), 0, 255));
            dst[x * 4 + 1] = static_cast<uint8_t>(std::clamp(int(std::round(g8 * a + 255.0f * (1.0f - a))), 0, 255));
            dst[x * 4 + 2] = static_cast<uint8_t>(std::clamp(int(std::round(r8 * a + 255.0f * (1.0f - a))), 0, 255));
            dst[x * 4 + 3] = 255;
        }
    }
    out.setColorSpace(QColorSpace::SRgb);
    error = S_OK;
    return out;
}

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

bool isRawExtension(const QString& ext) {
    static const QSet<QString> kRawExts = {
        "dng", "cr2", "cr3", "nef", "nrw", "arw", "srf", "sr2", "raf", "orf",
        "rw2", "pef", "srw", "erf", "kdc", "dcr", "3fr", "mef", "mos", "iiq", "raw"
    };
    return kRawExts.contains(ext);
}

QImage decodeCameraRaw(const QString& extension, const QByteArray& bytes, std::atomic<bool>& cancel, HRESULT& error, bool& outAlreadyWhiteSRgb, ImageExtendedInfo* ext) {
    const RawContainerInfo rawInfo = inspectRawContainer(bytes);
    if (ext && rawInfo.captureTimeSec > 0) {
        ext->captureTimeSec = rawInfo.captureTimeSec;
    }

    // 1. Try verified embedded JPEG previews in descending resolution order (up to 8 candidates)
    const size_t maxTries = std::min<size_t>(rawInfo.jpegs.size(), 8);
    for (size_t i = 0; i < maxTries; ++i) {
        if (cancel.load()) { error = HRESULT_FROM_WIN32(ERROR_CANCELLED); return {}; }
        const auto& cand = rawInfo.jpegs[i];
        QByteArray slice = QByteArray::fromRawData(bytes.constData() + cand.offset, cand.length);
        QBuffer buf(&slice);
        buf.open(QIODevice::ReadOnly);
        QImageReader jReader(&buf, "jpeg");
        jReader.setAutoTransform(true);
        const QSize jSize = jReader.size();
        if (!jSize.isValid() || jSize.width() <= 0 || jSize.height() <= 0) continue;
        const auto jTransform = jReader.transformation();

        QSize reportedSourceSize;
        if (jSize.width() > 16384 || jSize.height() > 16384 || uint64_t(jSize.width()) * jSize.height() > 67108864ULL) {
            if (jSize.width() <= 131072 && jSize.height() <= 131072 &&
                uint64_t(jSize.width()) * jSize.height() <= 1073741824ULL &&
                jReader.supportsOption(QImageIOHandler::ScaledSize)) {
                jReader.setScaledSize(jSize.scaled(4096, 4096, Qt::KeepAspectRatio));
                reportedSourceSize = jSize;
                if (jTransform & QImageIOHandler::TransformationRotate90) {
                    reportedSourceSize.transpose();
                }
            } else {
                continue;
            }
        }

        QImage jImg = jReader.read();
        if (!jImg.isNull()) {
            if (jTransform == QImageIOHandler::TransformationNone && rawInfo.exifOrientation > 1) {
                jImg = orient(std::move(jImg), rawInfo.exifOrientation);
                if (reportedSourceSize.isValid() && rawInfo.exifOrientation >= 5 && rawInfo.exifOrientation <= 8) {
                    reportedSourceSize.transpose();
                }
            }
            if (rawInfo.maxIfdWidth > 0 && rawInfo.maxIfdHeight > 0) {
                QSize sensorSize(int(rawInfo.maxIfdWidth), int(rawInfo.maxIfdHeight));
                if ((rawInfo.exifOrientation >= 5 && rawInfo.exifOrientation <= 8) ||
                    (rawInfo.exifOrientation <= 1 && (jTransform & QImageIOHandler::TransformationRotate90))) {
                    sensorSize.transpose();
                }
                const QSize effectivePreviewSize = reportedSourceSize.isValid() ? reportedSourceSize : jImg.size();
                if (uint64_t(sensorSize.width()) * uint64_t(sensorSize.height()) >
                    uint64_t(effectivePreviewSize.width()) * uint64_t(effectivePreviewSize.height())) {
                    reportedSourceSize = sensorSize;
                }
            }
            if (ext) {
                if (reportedSourceSize.isValid()) ext->sourceSize = reportedSourceSize;
                ext->kind = ImageEvidenceKind::RawEmbeddedPreview;
                ext->frameOrPageCount = 1;
            }
            outAlreadyWhiteSRgb = false;
            return jImg;
        }
    }

    // 2. If IFD0 is a reduced-resolution thumbnail, try platform WIC RAW developer first before falling back to IFD0
    auto tryWicRaw = [&]() -> QImage {
        QImage wicRaw = decodeWithWic(bytes, cancel, error);
        if (!wicRaw.isNull()) {
            if (ext) {
                ext->kind = ImageEvidenceKind::RawDeveloped;
                ext->frameOrPageCount = 1;
            }
            outAlreadyWhiteSRgb = true;
            return wicRaw;
        }
        return {};
    };
    if (rawInfo.ifd0ReducedResolution) {
        if (QImage wicImg = tryWicRaw(); !wicImg.isNull()) return wicImg;
    }

    // 3. Try uncompressed/linear DNG/TIFF IFD0, classifying reduced-res IFD0 thumbnails as RawEmbeddedPreview
    if (extension == "dng" || bytes.startsWith(QByteArrayView("II*\0", 4)) || bytes.startsWith(QByteArrayView("MM\0*", 4))) {
        QBuffer buf(const_cast<QByteArray*>(&bytes));
        buf.open(QIODevice::ReadOnly);
        QImageReader tReader(&buf, "tiff");
        tReader.setAutoTransform(true);
        const QSize tSize = tReader.size();
        const auto tTransform = tReader.transformation();
        if (tSize.isValid() && tSize.width() <= 16384 && tSize.height() <= 16384 &&
            uint64_t(tSize.width()) * tSize.height() <= 67108864ULL) {
            QImage tImg = tReader.read();
            if (!tImg.isNull()) {
                const bool isReducedPreview = rawInfo.ifd0ReducedResolution ||
                    (rawInfo.maxIfdWidth > uint32_t(tSize.width()) * 2 &&
                     rawInfo.maxIfdHeight > uint32_t(tSize.height()) * 2);
                if (ext) {
                    if (isReducedPreview && rawInfo.maxIfdWidth > 0 && rawInfo.maxIfdHeight > 0) {
                        QSize sensorSize(int(rawInfo.maxIfdWidth), int(rawInfo.maxIfdHeight));
                        if ((rawInfo.exifOrientation >= 5 && rawInfo.exifOrientation <= 8) ||
                            (rawInfo.exifOrientation <= 1 && (tTransform & QImageIOHandler::TransformationRotate90))) {
                            sensorSize.transpose();
                        }
                        if (uint64_t(sensorSize.width()) * uint64_t(sensorSize.height()) >
                            uint64_t(tImg.width()) * uint64_t(tImg.height())) {
                            ext->sourceSize = sensorSize;
                        }
                    }
                    ext->kind = isReducedPreview ? ImageEvidenceKind::RawEmbeddedPreview : ImageEvidenceKind::RawDeveloped;
                    ext->frameOrPageCount = 1;
                }
                outAlreadyWhiteSRgb = false;
                return tImg;
            }
        }
    }

    if (!rawInfo.ifd0ReducedResolution) {
        if (QImage wicImg = tryWicRaw(); !wicImg.isNull()) return wicImg;
    }

    // Distinguish valid RAW header with unsupported compression vs corrupt file
    if (bytes.startsWith(QByteArrayView("II*\0", 4)) || bytes.startsWith(QByteArrayView("MM\0*", 4)) ||
        bytes.startsWith("IIRO") || bytes.startsWith("IIRS") || bytes.startsWith("MMOR") ||
        bytes.startsWith(QByteArrayView("IIU\0", 4)) || bytes.startsWith("FUJIFILMCCD-RAW ") ||
        (bytes.size() >= 12 && QByteArray(bytes.constData() + 4, 8) == "ftypcrx ")) {
        error = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    } else {
        error = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }
    return {};
}

} // namespace StillImages::Internal
