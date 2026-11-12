#include "ImageReaderInternal.h"
#include <QColorSpace>
#include <QtEndian>
#include <bit>
#include <cmath>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

namespace StillImages::Internal {

namespace {

float halfBitsToFloat(uint16_t h) {
    const uint32_t sign = (h >> 15) & 1u;
    const uint32_t exp = (h >> 10) & 0x1Fu;
    const uint32_t mant = h & 0x3FFu;
    if (exp == 0) {
        const float v = static_cast<float>(mant) * (1.0f / 16777216.0f);
        return sign ? -v : v;
    }
    if (exp == 31) {
        return (mant == 0 && !sign) ? 65504.0f : 0.0f;
    }
    const uint32_t fbits = (sign << 31) | ((exp + 112u) << 23) | (mant << 13);
    return std::bit_cast<float>(fbits);
}

float readFloatSample(const uint8_t* ptr, bool isHalf, bool isBigEndian) {
    if (isHalf) {
        const uint16_t bits = isBigEndian ? qFromBigEndian<quint16>(ptr) : qFromLittleEndian<quint16>(ptr);
        return halfBitsToFloat(bits);
    }
    const uint32_t bits = isBigEndian ? qFromBigEndian<quint32>(ptr) : qFromLittleEndian<quint32>(ptr);
    return std::bit_cast<float>(bits);
}

float clampAlphaSample(float a) {
    return std::isfinite(a) ? std::clamp(a, 0.0f, 1.0f) : 1.0f;
}

} // namespace

uint8_t tonemapFloatToSrgbByte(float v) {
    if (!std::isfinite(v) || v <= 0.0f) return 0;
    const float mapped = v / (1.0f + v);
    const float srgb = (mapped <= 0.0031308f)
        ? (12.92f * mapped)
        : (1.055f * std::pow(mapped, 1.0f / 2.4f) - 0.055f);
    return static_cast<uint8_t>(std::clamp(int(std::round(srgb * 255.0f)), 0, 255));
}

QImage avFrameToWhiteSRgb(const AVFrame* frame, bool* outIsFloatHdr = nullptr) {
    if (!frame || frame->width <= 0 || frame->height <= 0 ||
        frame->width > 16384 || frame->height > 16384 ||
        uint64_t(frame->width) * frame->height > 67108864) {
        return {};
    }
    const int w = frame->width;
    const int h = frame->height;
    const auto fmt = static_cast<AVPixelFormat>(frame->format);

    if (fmt == AV_PIX_FMT_GBRPF32LE || fmt == AV_PIX_FMT_GBRPF32BE ||
        fmt == AV_PIX_FMT_GBRAPF32LE || fmt == AV_PIX_FMT_GBRAPF32BE ||
        fmt == AV_PIX_FMT_GBRPF16LE || fmt == AV_PIX_FMT_GBRPF16BE ||
        fmt == AV_PIX_FMT_GBRAPF16LE || fmt == AV_PIX_FMT_GBRAPF16BE) {
        if (!frame->data[0] || !frame->data[1] || !frame->data[2]) return {};
        if (outIsFloatHdr) *outIsFloatHdr = true;
        QImage out(w, h, QImage::Format_RGB32);
        if (out.isNull()) return {};
        const bool isHalf = (fmt == AV_PIX_FMT_GBRPF16LE || fmt == AV_PIX_FMT_GBRPF16BE ||
                             fmt == AV_PIX_FMT_GBRAPF16LE || fmt == AV_PIX_FMT_GBRAPF16BE);
        const bool isBe = (fmt == AV_PIX_FMT_GBRPF32BE || fmt == AV_PIX_FMT_GBRAPF32BE ||
                           fmt == AV_PIX_FMT_GBRPF16BE || fmt == AV_PIX_FMT_GBRAPF16BE);
        const bool wantAlpha = (fmt == AV_PIX_FMT_GBRAPF32LE || fmt == AV_PIX_FMT_GBRAPF32BE ||
                                fmt == AV_PIX_FMT_GBRAPF16LE || fmt == AV_PIX_FMT_GBRAPF16BE);
        const bool hasAlpha = (wantAlpha && frame->data[3]);
        const int step = isHalf ? 2 : 4;
        for (int y = 0; y < h; ++y) {
            const uint8_t* gRow = frame->data[0] + int64_t(y) * frame->linesize[0];
            const uint8_t* bRow = frame->data[1] + int64_t(y) * frame->linesize[1];
            const uint8_t* rRow = frame->data[2] + int64_t(y) * frame->linesize[2];
            const uint8_t* aRow = hasAlpha ? (frame->data[3] + int64_t(y) * frame->linesize[3]) : nullptr;
            uint8_t* dst = out.scanLine(y);
            for (int x = 0; x < w; ++x) {
                const int off = x * step;
                const float a = aRow ? clampAlphaSample(readFloatSample(aRow + off, isHalf, isBe)) : 1.0f;
                const uint8_t r8 = tonemapFloatToSrgbByte(readFloatSample(rRow + off, isHalf, isBe));
                const uint8_t g8 = tonemapFloatToSrgbByte(readFloatSample(gRow + off, isHalf, isBe));
                const uint8_t b8 = tonemapFloatToSrgbByte(readFloatSample(bRow + off, isHalf, isBe));
                dst[x * 4 + 0] = static_cast<uint8_t>(std::clamp(int(std::round(b8 * a + 255.0f * (1.0f - a))), 0, 255));
                dst[x * 4 + 1] = static_cast<uint8_t>(std::clamp(int(std::round(g8 * a + 255.0f * (1.0f - a))), 0, 255));
                dst[x * 4 + 2] = static_cast<uint8_t>(std::clamp(int(std::round(r8 * a + 255.0f * (1.0f - a))), 0, 255));
                dst[x * 4 + 3] = 255;
            }
        }
        out.setColorSpace(QColorSpace::SRgb);
        return out;
    }

    if (fmt == AV_PIX_FMT_GRAYF32LE || fmt == AV_PIX_FMT_GRAYF32BE ||
        fmt == AV_PIX_FMT_GRAYF16LE || fmt == AV_PIX_FMT_GRAYF16BE ||
        fmt == AV_PIX_FMT_YAF32LE || fmt == AV_PIX_FMT_YAF32BE ||
        fmt == AV_PIX_FMT_YAF16LE || fmt == AV_PIX_FMT_YAF16BE) {
        if (!frame->data[0]) return {};
        if (outIsFloatHdr) *outIsFloatHdr = true;
        QImage out(w, h, QImage::Format_RGB32);
        if (out.isNull()) return {};
        const bool isHalf = (fmt == AV_PIX_FMT_GRAYF16LE || fmt == AV_PIX_FMT_GRAYF16BE ||
                             fmt == AV_PIX_FMT_YAF16LE || fmt == AV_PIX_FMT_YAF16BE);
        const bool isBe = (fmt == AV_PIX_FMT_GRAYF32BE || fmt == AV_PIX_FMT_GRAYF16BE ||
                           fmt == AV_PIX_FMT_YAF32BE || fmt == AV_PIX_FMT_YAF16BE);
        const int ch = (fmt == AV_PIX_FMT_YAF32LE || fmt == AV_PIX_FMT_YAF32BE ||
                        fmt == AV_PIX_FMT_YAF16LE || fmt == AV_PIX_FMT_YAF16BE) ? 2 : 1;
        const int step = isHalf ? 2 : 4;
        for (int y = 0; y < h; ++y) {
            const uint8_t* yRow = frame->data[0] + int64_t(y) * frame->linesize[0];
            uint8_t* dst = out.scanLine(y);
            for (int x = 0; x < w; ++x) {
                const uint8_t* px = yRow + x * ch * step;
                const uint8_t v = tonemapFloatToSrgbByte(readFloatSample(px, isHalf, isBe));
                const float a = (ch == 2) ? clampAlphaSample(readFloatSample(px + step, isHalf, isBe)) : 1.0f;
                const uint8_t outV = (ch == 2)
                    ? static_cast<uint8_t>(std::clamp(int(std::round(v * a + 255.0f * (1.0f - a))), 0, 255))
                    : v;
                dst[x * 4 + 0] = outV;
                dst[x * 4 + 1] = outV;
                dst[x * 4 + 2] = outV;
                dst[x * 4 + 3] = 255;
            }
        }
        out.setColorSpace(QColorSpace::SRgb);
        return out;
    }

    if (fmt == AV_PIX_FMT_RGBF32LE || fmt == AV_PIX_FMT_RGBF32BE ||
        fmt == AV_PIX_FMT_RGBAF32LE || fmt == AV_PIX_FMT_RGBAF32BE ||
        fmt == AV_PIX_FMT_RGBF16LE || fmt == AV_PIX_FMT_RGBF16BE ||
        fmt == AV_PIX_FMT_RGBAF16LE || fmt == AV_PIX_FMT_RGBAF16BE) {
        if (!frame->data[0]) return {};
        if (outIsFloatHdr) *outIsFloatHdr = true;
        const bool isHalf = (fmt == AV_PIX_FMT_RGBF16LE || fmt == AV_PIX_FMT_RGBF16BE ||
                             fmt == AV_PIX_FMT_RGBAF16LE || fmt == AV_PIX_FMT_RGBAF16BE);
        const bool isBe = (fmt == AV_PIX_FMT_RGBF32BE || fmt == AV_PIX_FMT_RGBAF32BE ||
                           fmt == AV_PIX_FMT_RGBF16BE || fmt == AV_PIX_FMT_RGBAF16BE);
        const int ch = (fmt == AV_PIX_FMT_RGBAF32LE || fmt == AV_PIX_FMT_RGBAF32BE ||
                        fmt == AV_PIX_FMT_RGBAF16LE || fmt == AV_PIX_FMT_RGBAF16BE) ? 4 : 3;
        const int step = isHalf ? 2 : 4;
        QImage out(w, h, QImage::Format_RGB32);
        if (out.isNull()) return {};
        for (int y = 0; y < h; ++y) {
            const uint8_t* src = frame->data[0] + int64_t(y) * frame->linesize[0];
            uint8_t* dst = out.scanLine(y);
            for (int x = 0; x < w; ++x) {
                const uint8_t* px = src + x * ch * step;
                const float a = (ch == 4) ? clampAlphaSample(readFloatSample(px + 3 * step, isHalf, isBe)) : 1.0f;
                const uint8_t r8 = tonemapFloatToSrgbByte(readFloatSample(px + 0 * step, isHalf, isBe));
                const uint8_t g8 = tonemapFloatToSrgbByte(readFloatSample(px + 1 * step, isHalf, isBe));
                const uint8_t b8 = tonemapFloatToSrgbByte(readFloatSample(px + 2 * step, isHalf, isBe));
                dst[x * 4 + 0] = static_cast<uint8_t>(std::clamp(int(std::round(b8 * a + 255.0f * (1.0f - a))), 0, 255));
                dst[x * 4 + 1] = static_cast<uint8_t>(std::clamp(int(std::round(g8 * a + 255.0f * (1.0f - a))), 0, 255));
                dst[x * 4 + 2] = static_cast<uint8_t>(std::clamp(int(std::round(r8 * a + 255.0f * (1.0f - a))), 0, 255));
                dst[x * 4 + 3] = 255;
            }
        }
        out.setColorSpace(QColorSpace::SRgb);
        return out;
    }

    SwsContext* sws = sws_getContext(w, h, fmt, w, h, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws) return {};
    QImage rgba(w, h, QImage::Format_ARGB32);
    if (rgba.isNull()) { sws_freeContext(sws); return {}; }
    uint8_t* dstData[4] = { rgba.bits(), nullptr, nullptr, nullptr };
    int dstLinesize[4] = { int(rgba.bytesPerLine()), 0, 0, 0 };
    sws_scale(sws, frame->data, frame->linesize, 0, h, dstData, dstLinesize);
    sws_freeContext(sws);
    rgba.setColorSpace(QColorSpace::SRgb);
    return toWhiteSRgb(std::move(rgba));
}

QImage decodePacketWithFFmpegCodec(AVCodecID codecId, const QByteArray& bytes, std::atomic<bool>& cancel, HRESULT& error, bool* outIsHdr) {
    if (cancel.load()) { error = HRESULT_FROM_WIN32(ERROR_CANCELLED); return {}; }
    const AVCodec* codec = avcodec_find_decoder(codecId);
    if (!codec) {
        error = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        return {};
    }
    std::unique_ptr<AVCodecContext, void(*)(AVCodecContext*)> ctx(
        avcodec_alloc_context3(codec),
        [](AVCodecContext* c) { if (c) avcodec_free_context(&c); });
    if (!ctx) { error = E_OUTOFMEMORY; return {}; }
    ctx->max_pixels = 67108864;
    ctx->thread_count = 2;
    if (avcodec_open2(ctx.get(), codec, nullptr) < 0) return {};

    std::unique_ptr<AVPacket, void(*)(AVPacket*)> pkt(
        av_packet_alloc(),
        [](AVPacket* p) { if (p) av_packet_free(&p); });
    std::unique_ptr<AVFrame, void(*)(AVFrame*)> frame(
        av_frame_alloc(),
        [](AVFrame* f) { if (f) av_frame_free(&f); });
    if (!pkt || !frame) { error = E_OUTOFMEMORY; return {}; }

    pkt->data = reinterpret_cast<uint8_t*>(const_cast<char*>(bytes.constData()));
    pkt->size = int(bytes.size());

    if (avcodec_send_packet(ctx.get(), pkt.get()) < 0) return {};
    avcodec_send_packet(ctx.get(), nullptr);
    if (avcodec_receive_frame(ctx.get(), frame.get()) < 0) return {};

    QImage img = avFrameToWhiteSRgb(frame.get(), outIsHdr);
    if (!img.isNull()) error = S_OK;
    return img;
}

bool isValidTgaHeader(const QByteArray& bytes) {
    if (bytes.size() < 18) return false;
    const uint8_t cmapType = static_cast<uint8_t>(bytes[1]);
    const uint8_t imgType = static_cast<uint8_t>(bytes[2]);
    const uint16_t w = qFromLittleEndian<quint16>(bytes.constData() + 12);
    const uint16_t h = qFromLittleEndian<quint16>(bytes.constData() + 14);
    const uint8_t bpp = static_cast<uint8_t>(bytes[16]);
    if (cmapType > 1) return false;
    if (imgType != 1 && imgType != 2 && imgType != 3 && imgType != 9 && imgType != 10 && imgType != 11) return false;
    if (w == 0 || h == 0 || w > 16384 || h > 16384 || uint64_t(w) * h > 67108864) return false;
    if (bpp != 8 && bpp != 15 && bpp != 16 && bpp != 24 && bpp != 32) return false;
    return true;
}

} // namespace StillImages::Internal
