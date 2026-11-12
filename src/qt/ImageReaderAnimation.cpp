#include "ImageReaderInternal.h"
#include <QColorSpace>
#include <QIODevice>
#include <QPainter>
#include <QtEndian>
#include <algorithm>
#include <array>
#include <map>

namespace StillImages::Internal {

namespace {

bool appendBoundedPlaybackFrame(std::vector<QImage>& frames, QImage frame, uint64_t& storedPixels, QSize& maxDim) {
    constexpr uint64_t kMaxPlaybackPixels = 96ULL * 1024 * 1024;
    auto fitToMax = [](const QImage& img, const QSize& limit) -> QImage {
        if (img.width() <= limit.width() && img.height() <= limit.height()) return img;
        return img.scaled(limit, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    };
    frame = fitToMax(frame, maxDim);
    if (frame.isNull()) return false;
    while (storedPixels + uint64_t(frame.width()) * frame.height() > kMaxPlaybackPixels &&
           maxDim.width() > 320 && maxDim.height() > 180) {
        maxDim = QSize(std::max(320, maxDim.width() / 2), std::max(180, maxDim.height() / 2));
        storedPixels = 0;
        for (auto& existing : frames) {
            existing = fitToMax(existing, maxDim);
            storedPixels += uint64_t(existing.width()) * existing.height();
        }
        frame = fitToMax(frame, maxDim);
    }
    if (!frames.empty() && storedPixels + uint64_t(frame.width()) * frame.height() > kMaxPlaybackPixels) {
        return false;
    }
    storedPixels += uint64_t(frame.width()) * frame.height();
    frames.push_back(std::move(frame));
    return true;
}

} // namespace

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
    bool stoppedAtFrameCap = false;

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
            if (declaredFrames < 2) {
                error = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
                return {};
            }
        } else if (type == "fcTL") {
            if (len != 26) return {};
            if (currentSpec.hasFctl && !currentSpec.idatStream.isEmpty()) {
                specs.push_back(std::move(currentSpec));
                currentSpec = {};
                if (specs.size() >= 600) {
                    stoppedAtFrameCap = true;
                    break;
                }
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
    if (!stoppedAtFrameCap && currentSpec.hasFctl && !currentSpec.idatStream.isEmpty()) {
        specs.push_back(std::move(currentSpec));
    }

    const bool frameLimitExceeded = stoppedAtFrameCap || (declaredFrames > 600);
    if (!frameLimitExceeded && (specs.size() < 2 || (declaredFrames > 0 && specs.size() != declaredFrames))) {
        error = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        return {};
    }
    if (specs.empty()) {
        error = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        return {};
    }

    constexpr uint64_t kMaxCumulativePixels = 512ULL * 1024 * 1024;
    const uint64_t canvasPixels = uint64_t(canvasW) * canvasH;
    const bool pixelLimitExceeded = (uint64_t(specs.size()) * canvasPixels > kMaxCumulativePixels);
    const bool exceedsComparisonLimit = frameLimitExceeded || pixelLimitExceeded;

    const QSize orientedSourceSize = (exifOrient >= 5 && exifOrient <= 8)
        ? QSize(int(canvasH), int(canvasW))
        : QSize(int(canvasW), int(canvasH));

    QImage canvas(canvasW, canvasH, QImage::Format_ARGB32_Premultiplied);
    if (canvas.isNull()) { error = E_OUTOFMEMORY; return {}; }
    canvas.fill(Qt::transparent);

    const QByteArray iendChunk = makePngChunk("IEND", {});
    auto decodeStepOntoCanvas = [&](const ApngFrameSpec& spec, QImage& prevCanvasOut) -> bool {
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
            return false;
        }
        if (subImg.colorSpace().isValid()) subImg.convertToColorSpace(QColorSpace::SRgb);
        if (spec.disposeOp == 2) prevCanvasOut = canvas.copy();
        {
            QPainter p(&canvas);
            p.setCompositionMode(spec.blendOp == 0 ? QPainter::CompositionMode_Source : QPainter::CompositionMode_SourceOver);
            p.drawImage(int(spec.x), int(spec.y), subImg);
        }
        return true;
    };
    auto materializeCanvas = [&]() -> QImage {
        QImage presented(canvasW, canvasH, QImage::Format_RGB32);
        if (presented.isNull()) return {};
        presented.fill(Qt::white);
        {
            QPainter p(&presented);
            p.drawImage(0, 0, canvas);
        }
        presented.setColorSpace(QColorSpace::SRgb);
        if (exifOrient > 1) presented = orient(std::move(presented), exifOrient);
        return presented;
    };
    auto applyDispose = [&](const ApngFrameSpec& spec, QImage& prevCanvas) {
        if (spec.disposeOp == 1) {
            QPainter p(&canvas);
            p.setCompositionMode(QPainter::CompositionMode_Clear);
            p.fillRect(int(spec.x), int(spec.y), int(spec.w), int(spec.h), Qt::transparent);
        } else if (spec.disposeOp == 2) {
            canvas = std::move(prevCanvas);
        }
    };
    auto specDuration100ns = [](const ApngFrameSpec& spec) -> int64_t {
        const uint32_t den = (spec.delayDen == 0) ? 100u : uint32_t(spec.delayDen);
        int64_t dur100ns = (int64_t(spec.delayNum) * 10000000LL) / den;
        return (dur100ns <= 0) ? 1000000LL : dur100ns;
    };

    if (ext.keepPlaybackFrames) {
        std::vector<QImage> playbackFrames;
        std::vector<int64_t> playbackDurations;
        uint64_t cumulativePixels = 0;
        uint64_t storedPixels = 0;
        QSize maxDim(2560, 1440);
        bool truncated = frameLimitExceeded;

        for (size_t i = 0; i < specs.size(); ++i) {
            if (cancel.load()) { error = HRESULT_FROM_WIN32(ERROR_CANCELLED); return {}; }
            if (i > 0 && cumulativePixels + canvasPixels > kMaxCumulativePixels) {
                truncated = true;
                break;
            }
            cumulativePixels += canvasPixels;
            QImage prevCanvas;
            if (!decodeStepOntoCanvas(specs[i], prevCanvas)) return {};
            QImage presented = materializeCanvas();
            if (presented.isNull()) { error = E_OUTOFMEMORY; return {}; }
            if (!appendBoundedPlaybackFrame(playbackFrames, std::move(presented), storedPixels, maxDim)) {
                truncated = true;
                break;
            }
            playbackDurations.push_back(specDuration100ns(specs[i]));
            applyDispose(specs[i], prevCanvas);
        }
        if (playbackFrames.empty()) return {};
        int64_t totalDur = 0;
        for (int64_t d : playbackDurations) totalDur += d;
        ext.comparisonTruncated = truncated;
        ext.sourceSize = orientedSourceSize;
        ext.kind = (playbackFrames.size() >= 2 || declaredFrames >= 2) ? ImageEvidenceKind::AnimatedImage : ImageEvidenceKind::StaticImage;
        ext.frameOrPageCount = std::max<uint32_t>(declaredFrames, static_cast<uint32_t>(playbackFrames.size()));
        ext.totalDuration100ns = totalDur;
        ext.fps = (totalDur > 0) ? (double(playbackFrames.size()) * 10000000.0 / double(totalDur)) : 10.0;
        const size_t retIdx = (ext.requestedIndex >= 0) ? (size_t(ext.requestedIndex) % playbackFrames.size()) : 0;
        QImage retImg = playbackFrames[retIdx];
        ext.playbackFrames = std::move(playbackFrames);
        ext.frameDurations = std::move(playbackDurations);
        error = S_OK;
        return retImg;
    }

    // Analysis / single-frame path (keepPlaybackFrames == false)
    if (exceedsComparisonLimit) {
        QImage prevCanvas;
        if (decodeStepOntoCanvas(specs[0], prevCanvas)) {
            QImage firstFrame = materializeCanvas();
            if (!firstFrame.isNull()) {
                ext.comparisonTruncated = true;
                ext.previewFrame = std::move(firstFrame);
                ext.sourceSize = orientedSourceSize;
                ext.kind = ImageEvidenceKind::AnimatedImage;
                ext.frameOrPageCount = std::max<uint32_t>(declaredFrames, static_cast<uint32_t>(specs.size()));
            }
        }
        error = HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
        return {};
    }

    std::vector<int64_t> durations100ns;
    durations100ns.reserve(specs.size());
    int64_t totalDur = 0;
    for (const auto& spec : specs) {
        const int64_t d = specDuration100ns(spec);
        durations100ns.push_back(d);
        totalDur += d;
    }
    const auto sampleIndices = selectTemporalSamples(durations100ns);
    const size_t retIdx = (ext.requestedIndex >= 0) ? (size_t(ext.requestedIndex) % specs.size()) : 0;

    std::vector<bool> needed(specs.size(), false);
    needed[retIdx] = true;
    size_t maxNeededIdx = retIdx;
    if (ext.requestedIndex < 0) {
        for (size_t sIdx : sampleIndices) {
            needed[sIdx] = true;
            maxNeededIdx = std::max(maxNeededIdx, sIdx);
        }
        maxNeededIdx = specs.size() - 1; // validate full stream integrity during analyze
    }

    std::map<size_t, QImage> keptFrames;
    for (size_t i = 0; i <= maxNeededIdx; ++i) {
        if (cancel.load()) { error = HRESULT_FROM_WIN32(ERROR_CANCELLED); return {}; }
        QImage prevCanvas;
        if (!decodeStepOntoCanvas(specs[i], prevCanvas)) return {};
        if (needed[i]) {
            QImage presented = materializeCanvas();
            if (presented.isNull()) { error = E_OUTOFMEMORY; return {}; }
            keptFrames[i] = std::move(presented);
        }
        applyDispose(specs[i], prevCanvas);
    }

    ext.sourceSize = orientedSourceSize;
    ext.kind = ImageEvidenceKind::AnimatedImage;
    ext.frameOrPageCount = static_cast<uint32_t>(specs.size());
    ext.totalDuration100ns = totalDur;
    ext.fps = (totalDur > 0) ? (double(specs.size()) * 10000000.0 / double(totalDur)) : 10.0;
    ext.sampledFrames.clear();
    if (ext.requestedIndex < 0) {
        for (size_t sIdx : sampleIndices) ext.sampledFrames.push_back(keptFrames[sIdx]);
    }
    error = S_OK;
    return std::move(keptFrames[retIdx]);
}

QImage decodeQtAnimated(QImageReader& reader, std::atomic<bool>& cancel, HRESULT& error, ImageExtendedInfo* ext) {
    if (!ext) {
        error = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        return {};
    }
    constexpr uint64_t kMaxCumulativePixels = 512ULL * 1024 * 1024;

    if (ext->keepPlaybackFrames) {
        std::vector<QImage> playbackFrames;
        std::vector<int64_t> playbackDurations;
        uint64_t cumulativePixels = 0;
        uint64_t storedPixels = 0;
        QSize maxDim(2560, 1440);
        QSize origSize;
        bool truncated = false;

        for (int idx = 0; idx < 600; ++idx) {
            if (cancel.load()) { error = HRESULT_FROM_WIN32(ERROR_CANCELLED); return {}; }
            QImage rawFrame = reader.read();
            if (rawFrame.isNull()) {
                if (idx == 0) return {};
                break;
            }
            if (rawFrame.width() <= 0 || rawFrame.height() <= 0 ||
                rawFrame.width() > 16384 || rawFrame.height() > 16384 ||
                uint64_t(rawFrame.width()) * rawFrame.height() > 67108864) {
                if (idx == 0) return {};
                break;
            }
            if (idx == 0) origSize = rawFrame.size();
            const uint64_t framePx = uint64_t(rawFrame.width()) * rawFrame.height();
            if (idx > 0 && cumulativePixels + framePx > kMaxCumulativePixels) {
                truncated = true;
                break;
            }
            cumulativePixels += framePx;
            QImage normFrame = toWhiteSRgb(std::move(rawFrame));
            if (normFrame.isNull()) {
                if (idx == 0) return {};
                break;
            }
            const int delayMs = reader.nextImageDelay();
            const int64_t dur100ns = (delayMs > 0 ? int64_t(delayMs) : 100LL) * 10000LL;
            if (!appendBoundedPlaybackFrame(playbackFrames, std::move(normFrame), storedPixels, maxDim)) {
                truncated = true;
                break;
            }
            playbackDurations.push_back(dur100ns);
            if (!reader.canRead()) break;
            if (idx == 599 && reader.canRead()) truncated = true;
        }
        if (playbackFrames.empty()) return {};
        int64_t totalDur = 0;
        for (int64_t d : playbackDurations) totalDur += d;
        ext->comparisonTruncated = truncated;
        ext->sourceSize = origSize;
        if (playbackFrames.size() >= 2 || truncated) {
            ext->kind = ImageEvidenceKind::AnimatedImage;
            const int declaredCount = reader.imageCount();
            ext->frameOrPageCount = (declaredCount > int(playbackFrames.size()))
                ? static_cast<uint32_t>(declaredCount)
                : static_cast<uint32_t>(playbackFrames.size());
            ext->totalDuration100ns = totalDur;
            ext->fps = (totalDur > 0) ? (double(playbackFrames.size()) * 10000000.0 / double(totalDur)) : 10.0;
        } else {
            ext->kind = ImageEvidenceKind::StaticImage;
            ext->frameOrPageCount = 1;
        }
        const size_t retIdx = (ext->requestedIndex >= 0) ? (size_t(ext->requestedIndex) % playbackFrames.size()) : 0;
        QImage retImg = playbackFrames[retIdx];
        ext->playbackFrames = std::move(playbackFrames);
        ext->frameDurations = std::move(playbackDurations);
        error = S_OK;
        return retImg;
    }

    // Analysis / single-frame path (keepPlaybackFrames == false): cap in-RAM frames to <= 16 in pass 1
    QIODevice* dev = reader.device();
    std::map<size_t, QImage> keptFrames;
    std::vector<int64_t> durations100ns;
    uint64_t cumulativePixels = 0;
    QSize origSize;
    constexpr int kInlineKeepLimit = 16;

    for (int idx = 0; idx < 600; ++idx) {
        if (cancel.load()) { error = HRESULT_FROM_WIN32(ERROR_CANCELLED); return {}; }
        QImage rawFrame = reader.read();
        if (rawFrame.isNull()) return {};
        if (rawFrame.width() <= 0 || rawFrame.height() <= 0 ||
            rawFrame.width() > 16384 || rawFrame.height() > 16384 ||
            uint64_t(rawFrame.width()) * rawFrame.height() > 67108864) {
            return {};
        }
        if (idx == 0) origSize = rawFrame.size();
        cumulativePixels += uint64_t(rawFrame.width()) * rawFrame.height();
        if (cumulativePixels > kMaxCumulativePixels) {
            if (keptFrames.count(0)) {
                ext->comparisonTruncated = true;
                ext->previewFrame = keptFrames[0];
                ext->sourceSize = origSize;
                ext->kind = ImageEvidenceKind::AnimatedImage;
                const int declaredCount = reader.imageCount();
                ext->frameOrPageCount = (declaredCount > idx + 1) ? uint32_t(declaredCount) : uint32_t(idx + 1);
            }
            error = HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
            return {};
        }
        const int delayMs = reader.nextImageDelay();
        const int64_t dur100ns = (delayMs > 0 ? int64_t(delayMs) : 100LL) * 10000LL;
        durations100ns.push_back(dur100ns);

        if (idx < kInlineKeepLimit || (ext->requestedIndex >= 0 && idx == ext->requestedIndex)) {
            QImage normFrame = toWhiteSRgb(std::move(rawFrame));
            if (normFrame.isNull()) return {};
            keptFrames[size_t(idx)] = std::move(normFrame);
        }
        if (!reader.canRead()) break;
        if (idx == 599 && reader.canRead()) {
            if (keptFrames.count(0)) {
                ext->comparisonTruncated = true;
                ext->previewFrame = keptFrames[0];
                ext->sourceSize = origSize;
                ext->kind = ImageEvidenceKind::AnimatedImage;
                const int declaredCount = reader.imageCount();
                ext->frameOrPageCount = (declaredCount > 600) ? uint32_t(declaredCount) : 601u;
            }
            error = HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
            return {};
        }
    }
    if (durations100ns.empty() || !keptFrames.count(0)) return {};

    const size_t totalFrames = durations100ns.size();
    const size_t retIdx = (ext->requestedIndex >= 0) ? (size_t(ext->requestedIndex) % totalFrames) : 0;

    if (totalFrames >= 2) {
        int64_t totalDur = 0;
        for (int64_t d : durations100ns) totalDur += d;
        ext->sourceSize = origSize;
        ext->kind = ImageEvidenceKind::AnimatedImage;
        ext->frameOrPageCount = static_cast<uint32_t>(totalFrames);
        ext->totalDuration100ns = totalDur;
        ext->fps = (totalDur > 0) ? (double(totalFrames) * 10000000.0 / double(totalDur)) : 10.0;

        const auto sampleIndices = selectTemporalSamples(durations100ns);
        std::vector<bool> missing(totalFrames, false);
        size_t maxMissing = 0;
        bool anyMissing = false;
        if (!keptFrames.count(retIdx)) {
            missing[retIdx] = true;
            maxMissing = std::max(maxMissing, retIdx);
            anyMissing = true;
        }
        if (ext->requestedIndex < 0) {
            for (size_t sIdx : sampleIndices) {
                if (!keptFrames.count(sIdx)) {
                    missing[sIdx] = true;
                    maxMissing = std::max(maxMissing, sIdx);
                    anyMissing = true;
                }
            }
        }
        if (anyMissing) {
            if (!dev || !dev->seek(0)) return {};
            QImageReader pass2(dev);
            pass2.setAutoTransform(true);
            pass2.setDecideFormatFromContent(true);
            for (size_t idx = 0; idx <= maxMissing; ++idx) {
                if (cancel.load()) { error = HRESULT_FROM_WIN32(ERROR_CANCELLED); return {}; }
                QImage rawFrame = pass2.read();
                if (rawFrame.isNull()) return {};
                if (missing[idx]) {
                    QImage normFrame = toWhiteSRgb(std::move(rawFrame));
                    if (normFrame.isNull()) return {};
                    keptFrames[idx] = std::move(normFrame);
                }
            }
        }
        ext->sampledFrames.clear();
        if (ext->requestedIndex < 0) {
            for (size_t sIdx : sampleIndices) ext->sampledFrames.push_back(keptFrames[sIdx]);
        }
    } else {
        ext->sourceSize = origSize;
        ext->kind = ImageEvidenceKind::StaticImage;
        ext->frameOrPageCount = 1;
    }
    return std::move(keptFrames[retIdx]);
}

} // namespace StillImages::Internal
