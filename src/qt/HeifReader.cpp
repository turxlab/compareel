#include "HeifReader.h"
#include "ImageReaderInternal.h"
#include <QColorSpace>
#include <QPainter>
#include <QtEndian>
#include <libheif/heif.h>
#include <libheif/heif_sequences.h>
#include <map>
#include <algorithm>
#include <cmath>

namespace StillImages {
namespace {

QImage compositeToWhiteSRgb(QImage image) { return flattenTransparency(std::move(image)); }

int64_t extractHeifCaptureTimeSec(heif_image_handle* handle) {
    if (!handle) return 0;
    heif_item_id exifId = 0;
    if (heif_image_handle_get_list_of_metadata_block_IDs(handle, "Exif", &exifId, 1) != 1) return 0;
    const size_t exifSize = heif_image_handle_get_metadata_size(handle, exifId);
    if (exifSize <= 10 || exifSize > 1024 * 1024) return 0;
    QByteArray meta(qsizetype(exifSize), Qt::Uninitialized);
    if (heif_image_handle_get_metadata(handle, exifId, meta.data()).code != heif_error_Ok) return 0;
    const quint32 tiffOffset = qFromBigEndian<quint32>(meta.constData());
    if (4ULL + uint64_t(tiffOffset) >= uint64_t(meta.size())) return 0;
    return Internal::exifCaptureTimeSec(meta.mid(4 + qsizetype(tiffOffset)));
}

bool appendBoundedHeifPlaybackFrame(std::vector<QImage>& frames, QImage frame, uint64_t& storedPixels, QSize& maxDim) {
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

enum class HdrTransfer { None, Pq, Hlg };

// HDR photos (PQ or HLG, usually BT.2020) are tone mapped to an SDR sRGB picture, so they can be compared and shown
// like any other image. This is a display rendering, not a colour-exact conversion; similarity is still visual evidence.
QImage toneMapHdr(const heif_image* decoded, int w, int h, HdrTransfer transfer, bool bt2020, std::atomic<bool>& cancel) {
    int stride = 0;
    const auto plane = heif_image_get_plane_readonly(decoded, heif_channel_interleaved, &stride);
    const int bits = heif_image_get_bits_per_pixel_range(decoded, heif_channel_interleaved);
    if (!plane || bits <= 8 || bits > 16 || stride < w * 8) return {};
    const double maxCode = double((1 << bits) - 1);
    // Linear light relative to SDR reference white (203 cd/m2), the common HDR-to-SDR anchor.
    constexpr double referenceWhite = 203.0, peak = 1000.0;
    auto pq = [](double v) {
        constexpr double m1 = 2610.0 / 16384, m2 = 2523.0 / 4096 * 128, c1 = 3424.0 / 4096, c2 = 2413.0 / 4096 * 32, c3 = 2392.0 / 4096 * 32;
        const double p = std::pow(std::clamp(v, 0.0, 1.0), 1.0 / m2);
        return 10000.0 * std::pow(std::max(p - c1, 0.0) / (c2 - c3 * p), 1.0 / m1);
    };
    auto hlg = [](double v) {
        constexpr double a = 0.17883277, b = 0.28466892, c = 0.55991073;
        v = std::clamp(v, 0.0, 1.0);
        return v <= 0.5 ? v * v / 3.0 : (std::exp((v - c) / a) + b) / 12.0;
    };
    auto srgb = [](double v) {
        v = std::clamp(v, 0.0, 1.0);
        return v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
    };
    const double white = peak / referenceWhite;
    QImage result(w, h, QImage::Format_RGBA8888);
    if (result.isNull()) return {};
    for (int y = 0; y < h; ++y) {
        if ((y & 63) == 0 && cancel.load()) return {};
        const auto* row = reinterpret_cast<const uint16_t*>(plane + size_t(y) * stride);
        auto* out = result.scanLine(y);
        for (int x = 0; x < w; ++x) {
            double rgb[3];
            for (int c = 0; c < 3; ++c) {
                const double code = row[x * 4 + c] / maxCode;
                rgb[c] = transfer == HdrTransfer::Pq ? pq(code) / referenceWhite : hlg(code) * peak / referenceWhite;
            }
            if (transfer == HdrTransfer::Hlg) {
                // HLG system gamma 1.2 at 1000 cd/m2, applied on luminance.
                const double luminance = 0.2627 * rgb[0] + 0.6780 * rgb[1] + 0.0593 * rgb[2];
                const double gain = luminance > 0 ? std::pow(luminance / white, 0.2) : 0;
                for (double& v : rgb) v *= gain;
            }
            if (bt2020) {
                const double r = 1.6605 * rgb[0] - 0.5876 * rgb[1] - 0.0728 * rgb[2];
                const double g = -0.1246 * rgb[0] + 1.1329 * rgb[1] - 0.0083 * rgb[2];
                const double b = -0.0182 * rgb[0] - 0.1006 * rgb[1] + 1.1187 * rgb[2];
                rgb[0] = r; rgb[1] = g; rgb[2] = b;
            }
            for (int c = 0; c < 3; ++c) {
                const double v = std::max(rgb[c], 0.0);
                const double mapped = v * (1.0 + v / (white * white)) / (1.0 + v); // extended Reinhard: peak maps to 1
                out[x * 4 + c] = uchar(std::lround(srgb(mapped) * 255.0));
            }
            out[x * 4 + 3] = uchar(row[x * 4 + 3] * 255 / int(maxCode));
        }
    }
    result.setColorSpace(QColorSpace::SRgb);
    return result;
}

QImage decodeHeifHandle(heif_image_handle* handle, std::atomic<bool>& cancel, HRESULT& error) {
    const int width = heif_image_handle_get_width(handle);
    const int height = heif_image_handle_get_height(handle);
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384 || uint64_t(width) * height > 67108864) return {};

    heif_color_profile_nclx* rawNclx = nullptr;
    heif_image_handle_get_nclx_color_profile(handle, &rawNclx);
    std::unique_ptr<heif_color_profile_nclx, decltype(&heif_nclx_color_profile_free)> nclx(rawNclx, heif_nclx_color_profile_free);
    HdrTransfer hdr = HdrTransfer::None; bool bt2020 = false;
    if (nclx && (int(nclx->transfer_characteristics) == 16 || int(nclx->transfer_characteristics) == 18)) {
        hdr = int(nclx->transfer_characteristics) == 16 ? HdrTransfer::Pq : HdrTransfer::Hlg;
        bt2020 = int(nclx->color_primaries) == 9;
    }

    const auto profileSize = heif_image_handle_get_raw_color_profile_size(handle);
    if (profileSize > 1024 * 1024) return {};
    QColorSpace color;
    if (profileSize) {
        QByteArray profile(qsizetype(profileSize), Qt::Uninitialized);
        if (heif_image_handle_get_raw_color_profile(handle, profile.data()).code != heif_error_Ok) return {};
        color = QColorSpace::fromIccProfile(profile);
        if (!color.isValid()) return {};
        if (color.transferFunction() == QColorSpace::TransferFunction::St2084 ||
            color.transferFunction() == QColorSpace::TransferFunction::Hlg) {
            hdr = color.transferFunction() == QColorSpace::TransferFunction::St2084 ? HdrTransfer::Pq : HdrTransfer::Hlg;
            bt2020 = color.primaries() == QColorSpace::Primaries::Bt2020;
        }
    }
    if (heif_image_handle_get_luma_bits_per_pixel(handle) > 8 && !nclx && !profileSize) {
        error = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        return {};
    }

    std::unique_ptr<heif_decoding_options, decltype(&heif_decoding_options_free)> options(
        heif_decoding_options_alloc(), heif_decoding_options_free);
    if (!options) {
        error = E_OUTOFMEMORY;
        return {};
    }
    options->strict_decoding = 1;
    options->num_codec_threads = 2;
    options->progress_user_data = &cancel;
    options->cancel_decoding = [](void* data) { return static_cast<std::atomic<bool>*>(data)->load() ? 1 : 0; };
    options->output_image_nclx_profile_passthrough = profileSize ? 1 : 0;

    heif_image* rawImage = nullptr;
    const auto chroma = hdr != HdrTransfer::None ? heif_chroma_interleaved_RRGGBBAA_LE : heif_chroma_interleaved_RGBA;
    if (heif_decode_image(handle, &rawImage, heif_colorspace_RGB, chroma, options.get()).code != heif_error_Ok) {
        if (cancel.load()) error = HRESULT_FROM_WIN32(ERROR_CANCELLED);
        return {};
    }
    std::unique_ptr<heif_image, decltype(&heif_image_release)> decoded(rawImage, heif_image_release);
    const int w = heif_image_get_width(decoded.get(), heif_channel_interleaved);
    const int h = heif_image_get_height(decoded.get(), heif_channel_interleaved);
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384 || uint64_t(w) * h > 67108864) return {};
    if (hdr != HdrTransfer::None) {
        QImage mapped = toneMapHdr(decoded.get(), w, h, hdr, bt2020, cancel);
        if (mapped.isNull()) error = cancel.load() ? HRESULT_FROM_WIN32(ERROR_CANCELLED) : HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        return mapped;
    }

    int stride = 0;
    const auto pixels = heif_image_get_plane_readonly(decoded.get(), heif_channel_interleaved, &stride);
    if (!pixels || stride < w * 4 || cancel.load()) {
        if (cancel.load()) error = HRESULT_FROM_WIN32(ERROR_CANCELLED);
        return {};
    }
    QImage result = QImage(pixels, w, h, stride,
        heif_image_is_premultiplied_alpha(decoded.get()) ? QImage::Format_RGBA8888_Premultiplied : QImage::Format_RGBA8888).copy();
    if (result.isNull()) {
        error = E_OUTOFMEMORY;
        return {};
    }
    result.setColorSpace(profileSize ? color : QColorSpace(QColorSpace::SRgb));
    return result;
}

std::vector<size_t> selectTemporalSampleIndices(const std::vector<int64_t>& durations100ns) {
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
    if (n >= 2 && indices[3] == indices[4] && indices[2] == indices[3] && indices[1] == indices[2] && indices[0] == indices[1]) {
        indices[4] = n - 1;
    }
    return indices;
}

bool ensureHeifRuntime() {
    struct Runtime { bool ready = heif_init(nullptr).code == heif_error_Ok; ~Runtime() { if (ready) heif_deinit(); } };
    static Runtime runtime;
    return runtime.ready;
}

} // namespace

int64_t readHeifCaptureTimeSec(QFile& input, std::atomic<bool>& cancel) {
    if (!ensureHeifRuntime() || cancel.load()) return 0;
    if (input.size() > 256LL * 1024 * 1024 || !input.seek(0)) return 0;
    const auto bytes = input.readAll();
    if (bytes.size() != input.size() || bytes.isEmpty()) return 0;
    const QByteArray mime = heif_get_file_mime_type(reinterpret_cast<const uint8_t*>(bytes.constData()), bytes.size());
    if (mime != "image/heic" && mime != "image/heif" && mime != "image/avif" &&
        mime != "image/heic-sequence" && mime != "image/heif-sequence" && mime != "image/avif-sequence") {
        return 0;
    }
    std::unique_ptr<heif_context, decltype(&heif_context_free)> context(heif_context_alloc(), heif_context_free);
    if (!context) return 0;
    auto limits = *heif_get_global_security_limits();
    limits.max_image_size_pixels = 67108864;
    limits.max_number_of_tiles = 1024;
    limits.max_items = 4096;
    limits.max_memory_block_size = 256ULL * 1024 * 1024;
    limits.max_total_memory = 512ULL * 1024 * 1024;
    limits.max_color_profile_size = 1024 * 1024;
    limits.max_sequence_frames = 65535;
    if (heif_context_set_security_limits(context.get(), &limits).code != heif_error_Ok) return 0;
    heif_context_set_max_decoding_threads(context.get(), 0);
    if (heif_context_read_from_memory_without_copy(context.get(), bytes.constData(), bytes.size(), nullptr).code != heif_error_Ok) return 0;
    if (heif_context_number_of_sequence_tracks(context.get()) > 0) return 0;
    const int numTopImages = heif_context_get_number_of_top_level_images(context.get());
    if (numTopImages == 1) {
        heif_image_handle* rawH = nullptr;
        if (heif_context_get_primary_image_handle(context.get(), &rawH).code != heif_error_Ok) return 0;
        std::unique_ptr<heif_image_handle, decltype(&heif_image_handle_release)> h(rawH, heif_image_handle_release);
        return extractHeifCaptureTimeSec(h.get());
    }
    if (numTopImages > 1 && numTopImages <= 256) {
        heif_item_id firstId = 0;
        if (heif_context_get_list_of_top_level_image_IDs(context.get(), &firstId, 1) < 1) return 0;
        heif_image_handle* rawH = nullptr;
        if (heif_context_get_image_handle(context.get(), firstId, &rawH).code != heif_error_Ok) return 0;
        std::unique_ptr<heif_image_handle, decltype(&heif_image_handle_release)> h(rawH, heif_image_handle_release);
        return extractHeifCaptureTimeSec(h.get());
    }
    return 0;
}

QImage readHeif(QFile& input, std::atomic<bool>& cancel, HRESULT& error, QByteArray& format, ImageExtendedInfo* ext) {
    error = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    if (!ensureHeifRuntime()) { error = E_FAIL; return {}; }
    if (cancel.load()) { error = HRESULT_FROM_WIN32(ERROR_CANCELLED); return {}; }
    if (input.size() > 256LL * 1024 * 1024 || !input.seek(0)) return {};
    const auto bytes = input.readAll();
    if (bytes.size() != input.size()) return {};
    const QByteArray mime = heif_get_file_mime_type(reinterpret_cast<const uint8_t*>(bytes.constData()), bytes.size());
    if (mime != "image/heic" && mime != "image/heif" && mime != "image/avif" &&
        mime != "image/heic-sequence" && mime != "image/heif-sequence" && mime != "image/avif-sequence") {
        error = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        return {};
    }

    std::unique_ptr<heif_context, decltype(&heif_context_free)> context(heif_context_alloc(), heif_context_free);
    if (!context) { error = E_OUTOFMEMORY; return {}; }
    auto limits = *heif_get_global_security_limits();
    limits.max_image_size_pixels = 67108864;
    limits.max_number_of_tiles = 1024;
    limits.max_items = 4096;
    limits.max_memory_block_size = 256ULL * 1024 * 1024;
    limits.max_total_memory = 512ULL * 1024 * 1024;
    limits.max_color_profile_size = 1024 * 1024;
    limits.max_sequence_frames = ext ? 65535 : 1;
    if (heif_context_set_security_limits(context.get(), &limits).code != heif_error_Ok) return {};
    heif_context_set_max_decoding_threads(context.get(), 0);
    if (heif_context_read_from_memory_without_copy(context.get(), bytes.constData(), bytes.size(), nullptr).code != heif_error_Ok) return {};

    const int numTopImages = heif_context_get_number_of_top_level_images(context.get());
    const int numTracks = heif_context_number_of_sequence_tracks(context.get());

    if (!ext) {
        if (numTopImages != 1 || numTracks != 0) {
            error = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
            return {};
        }
    }

    const QByteArray outFmt = (mime.startsWith("image/avif")) ? "avif" : "heic";

    // Animated AVIF / HEIF sequence track
    if (ext && numTracks > 0) {
        heif_track* rawTrack = heif_context_get_track(context.get(), 0);
        if (!rawTrack) {
            error = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            return {};
        }
        std::unique_ptr<heif_track, decltype(&heif_track_release)> track(rawTrack, heif_track_release);
        uint16_t tw = 0, th = 0;
        if (heif_track_get_image_resolution(track.get(), &tw, &th).code != heif_error_Ok ||
            tw == 0 || th == 0 || uint64_t(tw) * th > 67108864) {
            error = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            return {};
        }
        const uint32_t timescale = std::max<uint32_t>(1, heif_track_get_timescale(track.get()));
        std::unique_ptr<heif_decoding_options, decltype(&heif_decoding_options_free)> options(
            heif_decoding_options_alloc(), heif_decoding_options_free);
        if (!options) { error = E_OUTOFMEMORY; return {}; }
        options->strict_decoding = 1;
        options->num_codec_threads = 2;
        options->ignore_sequence_editlist = 1;
        options->progress_user_data = &cancel;
        options->cancel_decoding = [](void* data) { return static_cast<std::atomic<bool>*>(data)->load() ? 1 : 0; };

        auto decodeHeifTrackFrame = [&](heif_image* decoded, int w, int h) -> QImage {
            heif_color_profile_nclx* rawNclx = nullptr;
            heif_image_get_nclx_color_profile(decoded, &rawNclx);
            std::unique_ptr<heif_color_profile_nclx, decltype(&heif_nclx_color_profile_free)> nclx(rawNclx, heif_nclx_color_profile_free);
            if (nclx && (int(nclx->transfer_characteristics) == 16 || int(nclx->transfer_characteristics) == 18)) {
                error = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
                return {};
            }
            int stride = 0;
            const auto pixels = heif_image_get_plane_readonly(decoded, heif_channel_interleaved, &stride);
            if (!pixels || stride < w * 4) return {};
            QImage frameImg = QImage(pixels, w, h, stride,
                heif_image_is_premultiplied_alpha(decoded) ? QImage::Format_RGBA8888_Premultiplied : QImage::Format_RGBA8888).copy();
            frameImg.setColorSpace(QColorSpace::SRgb);
            frameImg = compositeToWhiteSRgb(std::move(frameImg));
            if (frameImg.isNull()) error = E_OUTOFMEMORY;
            return frameImg;
        };

        constexpr uint64_t kMaxCumulativePixels = 512ULL * 1024 * 1024;

        if (ext->keepPlaybackFrames) {
            std::vector<QImage> playbackFrames;
            std::vector<int64_t> playbackDurations;
            uint64_t cumulativePixels = 0;
            uint64_t storedPixels = 0;
            QSize maxDim(2560, 1440);
            QSize origSize{int(tw), int(th)};
            bool truncated = false;

            for (int frameIdx = 0; frameIdx <= 600; ++frameIdx) {
                if (cancel.load()) { error = HRESULT_FROM_WIN32(ERROR_CANCELLED); return {}; }
                heif_image* rawImg = nullptr;
                const auto err = heif_track_decode_next_image(track.get(), &rawImg, heif_colorspace_RGB, heif_chroma_interleaved_RGBA, options.get());
                if (err.code == heif_error_End_of_sequence) break;
                if (err.code != heif_error_Ok || !rawImg) {
                    if (cancel.load()) error = HRESULT_FROM_WIN32(ERROR_CANCELLED);
                    if (frameIdx == 0) return {};
                    break;
                }
                std::unique_ptr<heif_image, decltype(&heif_image_release)> decoded(rawImg, heif_image_release);
                if (frameIdx == 600) { truncated = true; break; }
                const int w = heif_image_get_width(decoded.get(), heif_channel_interleaved);
                const int h = heif_image_get_height(decoded.get(), heif_channel_interleaved);
                if (w <= 0 || h <= 0 || w > 16384 || h > 16384 || uint64_t(w) * h > 67108864) {
                    if (frameIdx == 0) return {};
                    break;
                }
                if (frameIdx == 0) origSize = QSize(w, h);
                if (frameIdx > 0 && cumulativePixels + uint64_t(w) * h > kMaxCumulativePixels) {
                    truncated = true;
                    break;
                }
                cumulativePixels += uint64_t(w) * h;
                QImage frameImg = decodeHeifTrackFrame(decoded.get(), w, h);
                if (frameImg.isNull()) {
                    if (frameIdx == 0) return {};
                    break;
                }
                const uint32_t ticks = heif_image_get_duration(decoded.get());
                const int64_t dur100ns = (ticks > 0)
                    ? std::max<int64_t>(1LL, (int64_t(ticks) * 10000000LL) / timescale)
                    : 1000000LL;
                if (!appendBoundedHeifPlaybackFrame(playbackFrames, std::move(frameImg), storedPixels, maxDim)) {
                    truncated = true;
                    break;
                }
                playbackDurations.push_back(dur100ns);
            }
            if (playbackFrames.empty()) {
                error = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
                return {};
            }
            int64_t totalDur = 0;
            for (int64_t d : playbackDurations) totalDur += d;
            ext->comparisonTruncated = truncated;
            ext->sourceSize = origSize;
            if (playbackFrames.size() >= 2 || truncated) {
                ext->kind = ImageEvidenceKind::AnimatedImage;
                ext->frameOrPageCount = static_cast<uint32_t>(playbackFrames.size());
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
            format = outFmt;
            error = S_OK;
            return retImg;
        }

        // Analysis / single-frame path (keepPlaybackFrames == false): cap in-RAM frames to <= 16 in pass 1
        std::map<size_t, QImage> keptFrames;
        std::vector<int64_t> durations100ns;
        uint64_t cumulativePixels = 0;
        QSize origSize{int(tw), int(th)};
        constexpr int kInlineKeepLimit = 16;

        for (int frameIdx = 0; frameIdx <= 600; ++frameIdx) {
            if (cancel.load()) { error = HRESULT_FROM_WIN32(ERROR_CANCELLED); return {}; }
            heif_image* rawImg = nullptr;
            const auto err = heif_track_decode_next_image(track.get(), &rawImg, heif_colorspace_RGB, heif_chroma_interleaved_RGBA, options.get());
            if (err.code == heif_error_End_of_sequence) break;
            if (err.code != heif_error_Ok || !rawImg) {
                if (cancel.load()) error = HRESULT_FROM_WIN32(ERROR_CANCELLED);
                return {};
            }
            std::unique_ptr<heif_image, decltype(&heif_image_release)> decoded(rawImg, heif_image_release);
            if (frameIdx == 600) {
                if (keptFrames.count(0)) {
                    ext->comparisonTruncated = true;
                    ext->previewFrame = keptFrames[0];
                    ext->sourceSize = origSize;
                    ext->kind = ImageEvidenceKind::AnimatedImage;
                    ext->frameOrPageCount = 601u;
                    format = outFmt;
                }
                error = HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
                return {};
            }
            const int w = heif_image_get_width(decoded.get(), heif_channel_interleaved);
            const int h = heif_image_get_height(decoded.get(), heif_channel_interleaved);
            if (w <= 0 || h <= 0 || w > 16384 || h > 16384 || uint64_t(w) * h > 67108864) return {};
            if (frameIdx == 0) origSize = QSize(w, h);
            cumulativePixels += uint64_t(w) * h;
            if (cumulativePixels > kMaxCumulativePixels) {
                if (keptFrames.count(0)) {
                    ext->comparisonTruncated = true;
                    ext->previewFrame = keptFrames[0];
                    ext->sourceSize = origSize;
                    ext->kind = ImageEvidenceKind::AnimatedImage;
                    ext->frameOrPageCount = uint32_t(frameIdx + 1);
                    format = outFmt;
                }
                error = HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
                return {};
            }
            // Always verify HDR transfer rejection even for skipped frames
            heif_color_profile_nclx* rawNclx = nullptr;
            heif_image_get_nclx_color_profile(decoded.get(), &rawNclx);
            std::unique_ptr<heif_color_profile_nclx, decltype(&heif_nclx_color_profile_free)> nclx(rawNclx, heif_nclx_color_profile_free);
            if (nclx && (int(nclx->transfer_characteristics) == 16 || int(nclx->transfer_characteristics) == 18)) {
                error = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
                return {};
            }
            if (frameIdx < kInlineKeepLimit || (ext->requestedIndex >= 0 && frameIdx == ext->requestedIndex)) {
                QImage frameImg = decodeHeifTrackFrame(decoded.get(), w, h);
                if (frameImg.isNull()) return {};
                keptFrames[size_t(frameIdx)] = std::move(frameImg);
            }
            const uint32_t ticks = heif_image_get_duration(decoded.get());
            const int64_t dur100ns = (ticks > 0)
                ? std::max<int64_t>(1LL, (int64_t(ticks) * 10000000LL) / timescale)
                : 1000000LL;
            durations100ns.push_back(dur100ns);
        }
        if (durations100ns.empty() || !keptFrames.count(0)) {
            error = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            return {};
        }

        const size_t totalFrames = durations100ns.size();
        const size_t retIdx = (ext->requestedIndex >= 0) ? (size_t(ext->requestedIndex) % totalFrames) : 0;
        int64_t totalDur = 0;
        for (int64_t d : durations100ns) totalDur += d;

        if (totalFrames >= 2) {
            ext->sourceSize = origSize;
            ext->kind = ImageEvidenceKind::AnimatedImage;
            ext->frameOrPageCount = static_cast<uint32_t>(totalFrames);
            ext->totalDuration100ns = totalDur;
            ext->fps = (totalDur > 0) ? (double(totalFrames) * 10000000.0 / double(totalDur)) : 10.0;
            const auto sampleIdx = selectTemporalSampleIndices(durations100ns);

            std::vector<bool> missing(totalFrames, false);
            size_t maxMissing = 0;
            bool anyMissing = false;
            if (!keptFrames.count(retIdx)) {
                missing[retIdx] = true;
                maxMissing = std::max(maxMissing, retIdx);
                anyMissing = true;
            }
            if (ext->requestedIndex < 0) {
                for (size_t sIdx : sampleIdx) {
                    if (!keptFrames.count(sIdx)) {
                        missing[sIdx] = true;
                        maxMissing = std::max(maxMissing, sIdx);
                        anyMissing = true;
                    }
                }
            }
            if (anyMissing) {
                heif_track* rawTrack2 = heif_context_get_track(context.get(), 0);
                if (!rawTrack2) return {};
                std::unique_ptr<heif_track, decltype(&heif_track_release)> track2(rawTrack2, heif_track_release);
                for (size_t idx = 0; idx <= maxMissing; ++idx) {
                    if (cancel.load()) { error = HRESULT_FROM_WIN32(ERROR_CANCELLED); return {}; }
                    heif_image* rawImg = nullptr;
                    const auto err = heif_track_decode_next_image(track2.get(), &rawImg, heif_colorspace_RGB, heif_chroma_interleaved_RGBA, options.get());
                    if (err.code != heif_error_Ok || !rawImg) return {};
                    std::unique_ptr<heif_image, decltype(&heif_image_release)> decoded(rawImg, heif_image_release);
                    if (missing[idx]) {
                        const int w = heif_image_get_width(decoded.get(), heif_channel_interleaved);
                        const int h = heif_image_get_height(decoded.get(), heif_channel_interleaved);
                        QImage frameImg = decodeHeifTrackFrame(decoded.get(), w, h);
                        if (frameImg.isNull()) return {};
                        keptFrames[idx] = std::move(frameImg);
                    }
                }
            }
            ext->sampledFrames.clear();
            if (ext->requestedIndex < 0) {
                for (size_t idx : sampleIdx) ext->sampledFrames.push_back(keptFrames[idx]);
            }
        } else {
            ext->sourceSize = origSize;
            ext->kind = ImageEvidenceKind::StaticImage;
            ext->frameOrPageCount = 1;
        }
        format = outFmt;
        error = S_OK;
        return std::move(keptFrames[retIdx]);
    }

    // Multi-image HEIF / AVIF container
    if (ext && numTopImages > 1) {
        if (numTopImages > 256) {
            error = HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
            return {};
        }
        std::vector<heif_item_id> ids(numTopImages);
        heif_context_get_list_of_top_level_image_IDs(context.get(), ids.data(), numTopImages);
        auto decodePage = [&](int idx) -> QImage {
            heif_image_handle* rawH = nullptr;
            if (heif_context_get_image_handle(context.get(), ids[idx], &rawH).code != heif_error_Ok) return {};
            std::unique_ptr<heif_image_handle, decltype(&heif_image_handle_release)> h(rawH, heif_image_handle_release);
            if (idx == 0 && ext->captureTimeSec <= 0) ext->captureTimeSec = extractHeifCaptureTimeSec(h.get());
            return compositeToWhiteSRgb(decodeHeifHandle(h.get(), cancel, error));
        };
        ext->kind = ImageEvidenceKind::MultiPageDocument;
        ext->frameOrPageCount = static_cast<uint32_t>(numTopImages);
        if (ext->requestedIndex >= 0) {
            const int target = std::clamp(ext->requestedIndex, 0, numTopImages - 1);
            QImage page = decodePage(target);
            if (page.isNull()) return {};
            format = outFmt;
            error = S_OK;
            return page;
        }
        std::map<int, QImage> decodedCache;
        ext->sampledFrames.clear();
        for (int k = 0; k < 5; ++k) {
            const int idx = (k * (numTopImages - 1)) / 4;
            auto it = decodedCache.find(idx);
            if (it == decodedCache.end()) {
                QImage page = decodePage(idx);
                if (page.isNull()) return {};
                it = decodedCache.emplace(idx, std::move(page)).first;
            }
            ext->sampledFrames.push_back(it->second);
        }
        format = outFmt;
        error = S_OK;
        return ext->sampledFrames[0];
    }

    if (numTopImages != 1) {
        error = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        return {};
    }

    heif_image_handle* rawHandle = nullptr;
    if (heif_context_get_primary_image_handle(context.get(), &rawHandle).code != heif_error_Ok) return {};
    std::unique_ptr<heif_image_handle, decltype(&heif_image_handle_release)> handle(rawHandle, heif_image_handle_release);
    if (ext) ext->captureTimeSec = extractHeifCaptureTimeSec(handle.get());
    QImage result = decodeHeifHandle(handle.get(), cancel, error);
    if (result.isNull()) return {};
    if (ext) {
        ext->kind = ImageEvidenceKind::StaticImage;
        ext->frameOrPageCount = 1;
    }
    format = outFmt;
    error = S_OK;
    return result;
}

} // namespace StillImages
