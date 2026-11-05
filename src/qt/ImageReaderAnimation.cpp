#include "ImageReaderInternal.h"
#include <QColorSpace>
#include <QPainter>
#include <QtEndian>

namespace StillImages::Internal {

std::vector<size_t> selectTemporalSamples(const std::vector<int64_t>& durations100ns) {
    const size_t n = durations100ns.size();
    if (n == 0) return {};
    std::vector<int64_t> cumStart(n, 0);
    int64_t total = 0;
    for (size_t i = 0; i < n; ++i) {
        cumStart[i] = total;
        total += std::max<int64_t>(1, durations100ns[i]);
    }
    std::vector<size_t> indices(5, 0);
    indices[0] = 0;
    indices[4] = n - 1;
    for (int k = 1; k <= 3; ++k) {
        const int64_t targetTime = (total * k) / 4;
        size_t byTime = 0;
        for (size_t i = 0; i < n; ++i) {
            if (cumStart[i] <= targetTime) byTime = i;
            else break;
        }
        const size_t byIndex = (k * (n - 1) + 2) / 4;
        indices[k] = (n <= 5) ? byIndex : byTime;
    }
    return indices;
}

quint32 crc32Png(const char* data, size_t len) {
    static const auto table = [] {
        std::array<quint32, 256> t{};
        for (quint32 i = 0; i < 256; ++i) {
            quint32 c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
        return t;
    }();
    quint32 c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        c = table[(c ^ static_cast<uint8_t>(data[i])) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

QByteArray makePngChunk(const char type[4], const QByteArray& payload) {
    QByteArray out(8 + payload.size() + 4, Qt::Uninitialized);
    qToBigEndian<quint32>(static_cast<quint32>(payload.size()), out.data());
    memcpy(out.data() + 4, type, 4);
    if (!payload.isEmpty()) memcpy(out.data() + 8, payload.constData(), payload.size());
    const quint32 crc = crc32Png(out.constData() + 4, 4 + size_t(payload.size()));
    qToBigEndian<quint32>(crc, out.data() + 8 + payload.size());
    return out;
}

QImage decodeApng(const QByteArray& bytes, int exifOrient, std::atomic<bool>& cancel, HRESULT& error, ImageExtendedInfo& ext) {
    static const QByteArray kPngSig = QByteArray::fromHex("89504e470d0a1a0a");
    if (bytes.size() < 33 || !bytes.startsWith(kPngSig)) return {};
    if (qFromBigEndian<quint32>(bytes.constData() + 8) != 13 || QByteArray(bytes.constData() + 12, 4) != "IHDR") return {};

    const QByteArray baseIhdr = bytes.mid(16, 13);
    const quint32 canvasW = qFromBigEndian<quint32>(baseIhdr.constData());
    const quint32 canvasH = qFromBigEndian<quint32>(baseIhdr.constData() + 4);
    if (canvasW == 0 || canvasH == 0 || canvasW > 16384 || canvasH > 16384 || uint64_t(canvasW) * canvasH > 67108864) return {};

    struct ApngFrameSpec {
        quint32 w = 0, h = 0, x = 0, y = 0;
        quint16 delayNum = 1, delayDen = 10;
        uint8_t disposeOp = 0, blendOp = 0;
        QByteArray idatStream;
        bool hasFctl = false;
    };

    QByteArray sharedAncillary;
    std::vector<ApngFrameSpec> specs;
    ApngFrameSpec currentSpec{};
    bool seenIdatOrFctl = false;
    quint32 declaredFrames = 0;

    qsizetype offset = 8;
    while (offset + 12 <= bytes.size()) {
        if (cancel.load()) { error = HRESULT_FROM_WIN32(ERROR_CANCELLED); return {}; }
        const quint32 len = qFromBigEndian<quint32>(bytes.constData() + offset);
        const QByteArray type(bytes.constData() + offset + 4, 4);
        const qsizetype next = offset + 12 + qsizetype(len);
        if (next > bytes.size()) return {};
        const QByteArray data = bytes.mid(offset + 8, len);

        if (type == "acTL") {
            if (len != 8) return {};
            declaredFrames = qFromBigEndian<quint32>(data.constData());
            if (declaredFrames < 2 || declaredFrames > 600) {
                error = (declaredFrames > 600) ? HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE) : HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
                return {};
            }
        } else if (type == "fcTL") {
            if (len != 26) return {};
            if (currentSpec.hasFctl && !currentSpec.idatStream.isEmpty()) {
                specs.push_back(std::move(currentSpec));
                currentSpec = {};
            }
            currentSpec.w = qFromBigEndian<quint32>(data.constData() + 4);
            currentSpec.h = qFromBigEndian<quint32>(data.constData() + 8);
            currentSpec.x = qFromBigEndian<quint32>(data.constData() + 12);
            currentSpec.y = qFromBigEndian<quint32>(data.constData() + 16);
            currentSpec.delayNum = qFromBigEndian<quint16>(data.constData() + 20);
            currentSpec.delayDen = qFromBigEndian<quint16>(data.constData() + 22);
            currentSpec.disposeOp = static_cast<uint8_t>(data[24]);
            currentSpec.blendOp = static_cast<uint8_t>(data[25]);
            if (currentSpec.w == 0 || currentSpec.h == 0 ||
                uint64_t(currentSpec.x) + currentSpec.w > canvasW ||
                uint64_t(currentSpec.y) + currentSpec.h > canvasH ||
                currentSpec.disposeOp > 2 || currentSpec.blendOp > 1) {
                return {};
            }
            currentSpec.hasFctl = true;
            seenIdatOrFctl = true;
        } else if (type == "IDAT") {
            seenIdatOrFctl = true;
            if (currentSpec.hasFctl) {
                currentSpec.idatStream.append(makePngChunk("IDAT", data));
            }
        } else if (type == "fdAT") {
            if (len <= 4 || !currentSpec.hasFctl) return {};
            currentSpec.idatStream.append(makePngChunk("IDAT", data.mid(4)));
        } else if (type == "PLTE" || type == "tRNS" || type == "gAMA" || type == "cHRM" || type == "sRGB" || type == "iCCP") {
            if (!seenIdatOrFctl) {
                sharedAncillary.append(bytes.mid(offset, 12 + qsizetype(len)));
            }
        } else if (type == "IEND") {
            break;
        }
        offset = next;
    }
    if (currentSpec.hasFctl && !currentSpec.idatStream.isEmpty()) {
        specs.push_back(std::move(currentSpec));
    }
    if (specs.size() < 2 || (declaredFrames > 0 && specs.size() != declaredFrames)) {
        error = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        return {};
    }

    uint64_t cumulativePixels = 0;
    constexpr uint64_t kMaxCumulativePixels = 512ULL * 1024 * 1024;
    QImage canvas(canvasW, canvasH, QImage::Format_ARGB32_Premultiplied);
    if (canvas.isNull()) { error = E_OUTOFMEMORY; return {}; }
    canvas.fill(Qt::transparent);

    const QByteArray iendChunk = makePngChunk("IEND", {});
    std::vector<QImage> frames;
    std::vector<int64_t> durations100ns;
    frames.reserve(specs.size());
    durations100ns.reserve(specs.size());

    for (const auto& spec : specs) {
        if (cancel.load()) { error = HRESULT_FROM_WIN32(ERROR_CANCELLED); return {}; }
        cumulativePixels += uint64_t(canvasW) * canvasH;
        if (cumulativePixels > kMaxCumulativePixels) {
            error = HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
            return {};
        }
        QByteArray frameIhdr = baseIhdr;
        qToBigEndian<quint32>(spec.w, frameIhdr.data());
        qToBigEndian<quint32>(spec.h, frameIhdr.data() + 4);

        QByteArray singlePng;
        singlePng.reserve(kPngSig.size() + 25 + sharedAncillary.size() + spec.idatStream.size() + 12);
        singlePng.append(kPngSig);
        singlePng.append(makePngChunk("IHDR", frameIhdr));
        singlePng.append(sharedAncillary);
        singlePng.append(spec.idatStream);
        singlePng.append(iendChunk);

        QImage subImg = QImage::fromData(singlePng, "PNG");
        if (subImg.isNull() || quint32(subImg.width()) != spec.w || quint32(subImg.height()) != spec.h) {
            return {};
        }
        if (subImg.colorSpace().isValid()) subImg.convertToColorSpace(QColorSpace::SRgb);

        QImage prevCanvas;
        if (spec.disposeOp == 2) prevCanvas = canvas.copy();

        {
            QPainter p(&canvas);
            p.setCompositionMode(spec.blendOp == 0 ? QPainter::CompositionMode_Source : QPainter::CompositionMode_SourceOver);
            p.drawImage(int(spec.x), int(spec.y), subImg);
        }

        QImage presented(canvasW, canvasH, QImage::Format_RGB32);
        if (presented.isNull()) { error = E_OUTOFMEMORY; return {}; }
        presented.fill(Qt::white);
        {
            QPainter p(&presented);
            p.drawImage(0, 0, canvas);
        }
        presented.setColorSpace(QColorSpace::SRgb);
        if (exifOrient > 1) presented = orient(std::move(presented), exifOrient);
        frames.push_back(std::move(presented));

        const uint32_t den = (spec.delayDen == 0) ? 100u : uint32_t(spec.delayDen);
        int64_t dur100ns = (int64_t(spec.delayNum) * 10000000LL) / den;
        if (dur100ns <= 0) dur100ns = 1000000LL;
        durations100ns.push_back(dur100ns);

        if (spec.disposeOp == 1) {
            QPainter p(&canvas);
            p.setCompositionMode(QPainter::CompositionMode_Clear);
            p.fillRect(int(spec.x), int(spec.y), int(spec.w), int(spec.h), Qt::transparent);
        } else if (spec.disposeOp == 2) {
            canvas = std::move(prevCanvas);
        }
    }

    int64_t totalDur = 0;
    for (int64_t d : durations100ns) totalDur += d;
    ext.kind = ImageEvidenceKind::AnimatedImage;
    ext.frameOrPageCount = static_cast<uint32_t>(frames.size());
    ext.totalDuration100ns = totalDur;
    ext.fps = (totalDur > 0) ? (double(frames.size()) * 10000000.0 / double(totalDur)) : 10.0;
    const auto sampleIndices = selectTemporalSamples(durations100ns);
    ext.sampledFrames.clear();
    for (size_t idx : sampleIndices) ext.sampledFrames.push_back(frames[idx]);

    const size_t retIdx = (ext.requestedIndex >= 0) ? (size_t(ext.requestedIndex) % frames.size()) : 0;
    if(ext.keepPlaybackFrames) { ext.playbackFrames=frames; ext.frameDurations=durations100ns; }
    error = S_OK;
    return frames[retIdx];
}

QImage decodeQtAnimated(QImageReader& reader, std::atomic<bool>& cancel, HRESULT& error, ImageExtendedInfo* ext) {
        if (!ext) {
            error = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
            return {};
        }
        std::vector<QImage> frames;
        std::vector<int64_t> durations100ns;
        uint64_t cumulativePixels = 0;
        constexpr uint64_t kMaxCumulativePixels = 512ULL * 1024 * 1024;
        for (int idx = 0; idx < 600; ++idx) {
            if (cancel.load()) { error = HRESULT_FROM_WIN32(ERROR_CANCELLED); return {}; }
            QImage rawFrame = reader.read();
            if (rawFrame.isNull()) return {};
            if (rawFrame.width() <= 0 || rawFrame.height() <= 0 ||
                rawFrame.width() > 16384 || rawFrame.height() > 16384 ||
                uint64_t(rawFrame.width()) * rawFrame.height() > 67108864) {
                return {};
            }
            cumulativePixels += uint64_t(rawFrame.width()) * rawFrame.height();
            if (cumulativePixels > kMaxCumulativePixels) {
                error = HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
                return {};
            }
            QImage normFrame = toWhiteSRgb(std::move(rawFrame));
            if (normFrame.isNull()) return {};
            const int delayMs = reader.nextImageDelay();
            const int64_t dur100ns = (delayMs > 0 ? int64_t(delayMs) : 100LL) * 10000LL;
            frames.push_back(std::move(normFrame));
            durations100ns.push_back(dur100ns);
            if (!reader.canRead()) break;
        }
        if(frames.size()==600 && reader.canRead()) { error=HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE); return {}; }
        if (frames.empty()) return {};
        if (frames.size() >= 2) {
            int64_t totalDur = 0;
            for (int64_t d : durations100ns) totalDur += d;
            ext->kind = ImageEvidenceKind::AnimatedImage;
            ext->frameOrPageCount = static_cast<uint32_t>(frames.size());
            ext->totalDuration100ns = totalDur;
            ext->fps = (totalDur > 0) ? (double(frames.size()) * 10000000.0 / double(totalDur)) : 10.0;
            const auto sampleIndices = selectTemporalSamples(durations100ns);
            ext->sampledFrames.clear();
            for (size_t sIdx : sampleIndices) ext->sampledFrames.push_back(frames[sIdx]);
        } else {
            ext->kind = ImageEvidenceKind::StaticImage;
            ext->frameOrPageCount = 1;
        }
        const size_t retIdx = (ext->requestedIndex >= 0) ? (size_t(ext->requestedIndex) % frames.size()) : 0;
        if(ext->keepPlaybackFrames) { ext->playbackFrames=frames; ext->frameDurations=durations100ns; }
        return std::move(frames[retIdx]);
}

} // namespace StillImages::Internal
