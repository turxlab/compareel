#include "PerceptualHash.h"

namespace PerceptualHash {

SubjectRegionFeatures ComputeSubjectFeaturesForBox(
    const uint8_t* row0, uint32_t width, uint32_t height, int32_t stride,
    double boxX0, double boxY0, double boxX1, double boxY1)
{
    SubjectRegionFeatures out{};
    const double bw = std::max(8.0, boxX1 - boxX0);
    const double bh = std::max(8.0, boxY1 - boxY0);

    // 1. Sample 32 outer perimeter cells on a 9x9 grid of the box to estimate backdrop color & texture floor.
    struct PatchStat {
        double r = 0.0, g = 0.0, b = 0.0, y = 0.0, edge = 0.0, stdY = 0.0;
    };
    PatchStat perim[32]{};
    int pCount = 0;
    for (int gy = 0; gy < 9; ++gy) {
        for (int gx = 0; gx < 9; ++gx) {
            if (gy != 0 && gy != 8 && gx != 0 && gx != 8) continue;
            double cy0 = boxY0 + (gy * bh) / 9.0, cy1 = boxY0 + ((gy + 1) * bh) / 9.0;
            double cx0 = boxX0 + (gx * bw) / 9.0, cx1 = boxX0 + ((gx + 1) * bw) / 9.0;
            double subY[6][6]{};
            double sumR = 0.0, sumG = 0.0, sumB = 0.0, sumY = 0.0, sumYSq = 0.0;
            for (int sy = 0; sy < 6; ++sy) {
                double fy = cy0 + (2 * sy + 1) * (cy1 - cy0) / 12.0 - 0.5;
                for (int sx = 0; sx < 6; ++sx) {
                    double fx = cx0 + (2 * sx + 1) * (cx1 - cx0) / 12.0 - 0.5;
                    double r = 0.0, g = 0.0, b = 0.0, luma = 0.0;
                    SampleBilinearRGBY(row0, width, height, stride, fx, fy, r, g, b, luma);
                    subY[sy][sx] = luma;
                    sumR += r; sumG += g; sumB += b; sumY += luma; sumYSq += luma * luma;
                }
            }
            double edgeAcc = 0.0;
            int edgePairs = 0;
            for (int sy = 0; sy < 6; ++sy) {
                for (int sx = 0; sx < 6; ++sx) {
                    if (sx + 1 < 6) { edgeAcc += std::abs(subY[sy][sx] - subY[sy][sx + 1]); ++edgePairs; }
                    if (sy + 1 < 6) { edgeAcc += std::abs(subY[sy][sx] - subY[sy + 1][sx]); ++edgePairs; }
                }
            }
            auto& st = perim[pCount++];
            st.r = sumR / 36.0; st.g = sumG / 36.0; st.b = sumB / 36.0; st.y = sumY / 36.0;
            st.edge = edgePairs > 0 ? (edgeAcc / edgePairs) : 0.0;
            double varY = std::max(0.0, (sumYSq / 36.0) - (st.y * st.y));
            st.stdY = std::sqrt(varY);
        }
    }

    // Sort perimeter cells by luma and average the middle 50% (indices 8..23) for robust backdrop estimation.
    int order[32];
    for (int i = 0; i < pCount; ++i) order[i] = i;
    std::sort(order, order + pCount, [&](int a, int b) { return perim[a].y < perim[b].y; });
    double bgR = 0.0, bgG = 0.0, bgB = 0.0, bgY = 0.0, bgEdge = 0.0, bgStd = 0.0;
    int midBegin = pCount / 4, midEnd = pCount - (pCount / 4);
    int midCount = std::max(1, midEnd - midBegin);
    for (int i = midBegin; i < midEnd; ++i) {
        const auto& st = perim[order[i]];
        bgR += st.r; bgG += st.g; bgB += st.b; bgY += st.y; bgEdge += st.edge; bgStd += st.stdY;
    }
    bgR /= midCount; bgG /= midCount; bgB /= midCount; bgY /= midCount; bgEdge /= midCount; bgStd /= midCount;

    double bgSpread = 0.0;
    for (int i = midBegin; i < midEnd; ++i) {
        const auto& st = perim[order[i]];
        bgSpread += (std::abs(st.r - bgR) + std::abs(st.g - bgG) + std::abs(st.b - bgB)) / 3.0;
    }
    bgSpread /= midCount;
    out.bgCoherencePermille = static_cast<uint16_t>(std::clamp(std::lround(1000.0 - bgSpread * 35.0), 0L, 1000L));

    // 2. Sample symmetric 9x9 Subject ROI grid over [16% .. 84%] x [16% .. 84%] of the box.
    const double sx0 = boxX0 + bw * 0.16, sx1 = boxX0 + bw * 0.84;
    const double sy0 = boxY0 + bh * 0.16, sy1 = boxY0 + bh * 0.84;
    const double sw = std::max(4.0, sx1 - sx0), sh = std::max(4.0, sy1 - sy0);

    double sR[9][9]{}, sG[9][9]{}, sB[9][9]{}, sY[9][9]{};
    double sGx[9][9]{}, sGy[9][9]{}, sEdge[9][9]{}, sStd[9][9]{};

    for (int gy = 0; gy < 9; ++gy) {
        double cy0 = sy0 + (gy * sh) / 9.0, cy1 = sy0 + ((gy + 1) * sh) / 9.0;
        for (int gx = 0; gx < 9; ++gx) {
            double cx0 = sx0 + (gx * sw) / 9.0, cx1 = sx0 + ((gx + 1) * sw) / 9.0;
            double subY[4][4]{};
            double sumR = 0.0, sumG = 0.0, sumB = 0.0, sumY = 0.0, sumYSq = 0.0;
            for (int pyIdx = 0; pyIdx < 4; ++pyIdx) {
                double py0 = cy0 + pyIdx * (cy1 - cy0) / 4.0;
                double py1 = cy0 + (pyIdx + 1) * (cy1 - cy0) / 4.0;
                for (int pxIdx = 0; pxIdx < 4; ++pxIdx) {
                    double px0 = cx0 + pxIdx * (cx1 - cx0) / 4.0;
                    double px1 = cx0 + (pxIdx + 1) * (cx1 - cx0) / 4.0;
                    double pr = 0.0, pg = 0.0, pb = 0.0, pluma = 0.0;
                    for (int sy = 0; sy < 2; ++sy) {
                        double fy = py0 + (2 * sy + 1) * (py1 - py0) / 4.0 - 0.5;
                        for (int sx = 0; sx < 2; ++sx) {
                            double fx = px0 + (2 * sx + 1) * (px1 - px0) / 4.0 - 0.5;
                            double r = 0.0, g = 0.0, b = 0.0, luma = 0.0;
                            SampleBilinearRGBY(row0, width, height, stride, fx, fy, r, g, b, luma);
                            pr += r; pg += g; pb += b; pluma += luma;
                        }
                    }
                    pr *= 0.25; pg *= 0.25; pb *= 0.25; pluma *= 0.25;
                    subY[pyIdx][pxIdx] = pluma;
                    sumR += pr; sumG += pg; sumB += pb; sumY += pluma; sumYSq += pluma * pluma;
                }
            }
            sR[gy][gx] = sumR / 16.0;
            sG[gy][gx] = sumG / 16.0;
            sB[gy][gx] = sumB / 16.0;
            sY[gy][gx] = sumY / 16.0;
            double varY = std::max(0.0, (sumYSq / 16.0) - (sY[gy][gx] * sY[gy][gx]));
            sStd[gy][gx] = std::sqrt(varY);

            double accGx = 0.0, accGy = 0.0;
            for (int r = 0; r < 4; ++r) {
                for (int c = 0; c < 3; ++c) accGx += std::abs(subY[r][c] - subY[r][c + 1]);
            }
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 4; ++c) accGy += std::abs(subY[r][c] - subY[r + 1][c]);
            }
            sGx[gy][gx] = accGx / 12.0;
            sGy[gy][gx] = accGy / 12.0;
            sEdge[gy][gx] = sGx[gy][gx] + sGy[gy][gx];
        }
    }
    for (int gy = 0; gy < 8; ++gy) {
        for (int gx = 0; gx < 8; ++gx) {
            double macroGx = std::abs(sY[gy][gx] - sY[gy][gx + 1]);
            double macroGy = std::abs(sY[gy][gx] - sY[gy + 1][gx]);
            sGx[gy][gx] = 0.50 * macroGx + 0.50 * sGx[gy][gx];
            sGy[gy][gx] = 0.50 * macroGy + 0.50 * sGy[gy][gx];
            sEdge[gy][gx] = sGx[gy][gx] + sGy[gy][gx];
        }
    }

    // 3. Determine foreground activity mask (fgCellMask64) across the 8x8 subject cells.
    double saliency[64]{};
    const double cThresh = std::max(4.0, bgSpread * 1.35);
    const double eThresh = std::max(0.95, bgEdge * 0.28);
    const double sThresh = std::max(1.30, bgStd * 0.30);
    uint64_t fgMask64 = 0;

    for (int gy = 0; gy < 8; ++gy) {
        for (int gx = 0; gx < 8; ++gx) {
            int bit = gy * 8 + gx;
            double cDiff = std::max({
                std::abs(sR[gy][gx] - bgR),
                std::abs(sG[gy][gx] - bgG),
                std::abs(sB[gy][gx] - bgB),
                std::abs(sY[gy][gx] - bgY)
            });
            double eDiff = std::abs(sEdge[gy][gx] - bgEdge);
            double stdDiff = std::abs(sStd[gy][gx] - bgStd);
            saliency[bit] = cDiff + 3.5 * eDiff + 2.0 * stdDiff;
            if (cDiff > cThresh || eDiff > eThresh || stdDiff > sThresh) {
                fgMask64 |= (1ULL << bit);
            }
        }
    }

    if (std::popcount(fgMask64) < 16) {
        int sOrder[64];
        for (int i = 0; i < 64; ++i) sOrder[i] = i;
        std::sort(sOrder, sOrder + 64, [&](int a, int b) { return saliency[a] > saliency[b]; });
        for (int i = 0; i < 20; ++i) {
            if (saliency[sOrder[i]] > 0.8 || i < 12) fgMask64 |= (1ULL << sOrder[i]);
        }
    }
    out.fgCellMask64 = fgMask64;

    // 4. Compute foreground-normalized statistics & 64-bit structural/edge hashes.
    double fgSumY = 0.0, fgSumYSq = 0.0, fgSumGx = 0.0, fgSumGy = 0.0, fgSumEdge = 0.0;
    int fgCount = 0;
    for (int gy = 0; gy < 8; ++gy) {
        for (int gx = 0; gx < 8; ++gx) {
            int bit = gy * 8 + gx;
            if (fgMask64 & (1ULL << bit)) {
                double y = sY[gy][gx];
                fgSumY += y;
                fgSumYSq += y * y;
                fgSumGx += sGx[gy][gx];
                fgSumGy += sGy[gy][gx];
                fgSumEdge += sEdge[gy][gx];
                ++fgCount;
            }
        }
    }
    if (fgCount == 0) fgCount = 1;
    double fgMeanY = fgSumY / fgCount;
    double fgVarY = std::max(0.0, (fgSumYSq / fgCount) - (fgMeanY * fgMeanY));
    double fgStdY = std::sqrt(fgVarY);
    double fgMeanGx = fgSumGx / fgCount;
    double fgMeanGy = fgSumGy / fgCount;
    double fgMeanEdge = fgSumEdge / fgCount;

    out.subjStdY = fgStdY;
    out.subjMeanEdge = fgMeanEdge;

    double subjEps = std::clamp(fgStdY * 0.08, 0.20, 2.0);
    for (int gy = 0; gy < 8; ++gy) {
        for (int gx = 0; gx < 8; ++gx) {
            int bit = gy * 8 + gx;
            if (sY[gy][gx] > sY[gy][gx + 1] + subjEps) out.subjDHashH |= (1ULL << bit);
            if (sY[gy][gx] > sY[gy + 1][gx] + subjEps) out.subjDHashV |= (1ULL << bit);
            if (sY[gy][gx] >= fgMeanY) out.subjAHash |= (1ULL << bit);
            if (sGx[gy][gx] >= fgMeanGx + 0.08) out.edgeHashH |= (1ULL << bit);
            if (sGy[gy][gx] >= fgMeanGy + 0.08) out.edgeHashV |= (1ULL << bit);
            if (sEdge[gy][gx] >= fgMeanEdge + 0.12) out.edgeAHash |= (1ULL << bit);
        }
    }

    // 5. Pool 8x8 subject cells into 4x4 subject RGB grid and 16-bit foreground mask.
    for (int cy = 0; cy < 4; ++cy) {
        for (int cx = 0; cx < 4; ++cx) {
            int cellIdx = cy * 4 + cx;
            double accR = 0.0, accG = 0.0, accB = 0.0;
            double fgR = 0.0, fgG = 0.0, fgB = 0.0;
            int fgSub = 0;
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    int gy = cy * 2 + dy, gx = cx * 2 + dx;
                    accR += sR[gy][gx]; accG += sG[gy][gx]; accB += sB[gy][gx];
                    if (fgMask64 & (1ULL << (gy * 8 + gx))) {
                        fgR += sR[gy][gx]; fgG += sG[gy][gx]; fgB += sB[gy][gx];
                        ++fgSub;
                    }
                }
            }
            if (fgSub > 0) {
                out.subjFgMask |= static_cast<uint16_t>(1u << cellIdx);
                out.subjRgbGrid[cellIdx * 3 + 0] = static_cast<uint8_t>(std::clamp(std::lround(fgR / fgSub), 0L, 255L));
                out.subjRgbGrid[cellIdx * 3 + 1] = static_cast<uint8_t>(std::clamp(std::lround(fgG / fgSub), 0L, 255L));
                out.subjRgbGrid[cellIdx * 3 + 2] = static_cast<uint8_t>(std::clamp(std::lround(fgB / fgSub), 0L, 255L));
            } else {
                out.subjRgbGrid[cellIdx * 3 + 0] = static_cast<uint8_t>(std::clamp(std::lround(accR / 4.0), 0L, 255L));
                out.subjRgbGrid[cellIdx * 3 + 1] = static_cast<uint8_t>(std::clamp(std::lround(accG / 4.0), 0L, 255L));
                out.subjRgbGrid[cellIdx * 3 + 2] = static_cast<uint8_t>(std::clamp(std::lround(accB / 4.0), 0L, 255L));
            }
        }
    }

    return out;
}

// Compute FrameSignature from a BGRA32 buffer (width, height, stride in bytes)
FrameSignature ComputeSignatureFromBGRA(const uint8_t* data, uint32_t width, uint32_t height, int32_t stride, bool spatialDetail) {
    FrameSignature sig{};
    if (!data || width < 4 || height < 4 || std::abs(int64_t(stride)) < int64_t(width) * 4) return sig;

    const uint8_t* row0 = data;

    // 1. Sample 9x9 luminance grid (using box averaging over each cell)
    double lum9x9[9][9]{};
    double sumLum = 0.0;
    double sumLumSq = 0.0;

    for (int gy = 0; gy < 9; ++gy) {
        uint32_t y0 = (gy * height) / 9;
        uint32_t y1 = ((gy + 1) * height) / 9;
        if (y1 <= y0) y1 = y0 + 1;

        for (int gx = 0; gx < 9; ++gx) {
            uint32_t x0 = (gx * width) / 9;
            uint32_t x1 = ((gx + 1) * width) / 9;
            if (x1 <= x0) x1 = x0 + 1;

            uint64_t accY = 0;
            uint64_t count = 0;
            for (uint32_t sampleY=0;sampleY<8;++sampleY) {
                uint32_t y=std::min(height-1,y0+(2*sampleY+1)*(y1-y0)/16);
                const uint8_t* row = row0 + static_cast<int64_t>(y) * stride;
                for (uint32_t sampleX=0;sampleX<8;++sampleX) {
                    uint32_t x=std::min(width-1,x0+(2*sampleX+1)*(x1-x0)/16);
                    const uint8_t* px = row + static_cast<int64_t>(x) * 4;
                    uint32_t b = px[0];
                    uint32_t g = px[1];
                    uint32_t r = px[2];
                    // Fast BT.601 luma: (77*R + 150*G + 29*B) >> 8
                    accY += (77u * r + 150u * g + 29u * b) >> 8;
                    ++count;
                }
            }
            double val = count ? (static_cast<double>(accY) / static_cast<double>(count)) : 0.0;
            lum9x9[gy][gx] = val;
            sumLum += val;
            sumLumSq += val * val;
        }
    }

    double meanLum = sumLum / 81.0;
    double variance = (sumLumSq / 81.0) - (meanLum * meanLum);
    sig.lowVariance = (variance < 12.0); // Nearly uniform / solid black or white frame

    // 2. Horizontal dHash (8 rows x 8 diffs = 64 bits)
    uint64_t dHashH = 0;
    int bitH = 0;
    for (int gy = 0; gy < 8; ++gy) {
        for (int gx = 0; gx < 8; ++gx) {
            // Ignore tiny encode noise in otherwise flat gradients.
            if (lum9x9[gy][gx] > lum9x9[gy][gx + 1]+2.0) {
                dHashH |= (1ULL << bitH);
            }
            ++bitH;
        }
    }
    sig.dHashH = dHashH;

    // 3. Vertical dHash (8 rows diffs x 8 cols = 64 bits)
    uint64_t dHashV = 0;
    int bitV = 0;
    for (int gy = 0; gy < 8; ++gy) {
        for (int gx = 0; gx < 8; ++gx) {
            if (lum9x9[gy][gx] > lum9x9[gy + 1][gx]+2.0) {
                dHashV |= (1ULL << bitV);
            }
            ++bitV;
        }
    }
    sig.dHashV = dHashV;

    // 4. Average Hash (8x8 = 64 bits)
    double sum64 = 0.0;
    for (int gy = 0; gy < 8; ++gy) {
        for (int gx = 0; gx < 8; ++gx) {
            sum64 += lum9x9[gy][gx];
        }
    }
    double avg64 = sum64 / 64.0;
    uint64_t aHash = 0;
    int bitA = 0;
    for (int gy = 0; gy < 8; ++gy) {
        for (int gx = 0; gx < 8; ++gx) {
            if (lum9x9[gy][gx] >= avg64) {
                aHash |= (1ULL << bitA);
            }
            ++bitA;
        }
    }
    sig.aHash = aHash;

    // 5. 4x4 Spatial RGB grid (16 cells x 3 channels = 48 bytes)
    int idx = 0;
    for (int gy = 0; gy < 4; ++gy) {
        uint32_t y0 = (gy * height) / 4;
        uint32_t y1 = ((gy + 1) * height) / 4;
        if (y1 <= y0) y1 = y0 + 1;

        for (int gx = 0; gx < 4; ++gx) {
            uint32_t x0 = (gx * width) / 4;
            uint32_t x1 = ((gx + 1) * width) / 4;
            if (x1 <= x0) x1 = x0 + 1;

            uint64_t accR = 0, accG = 0, accB = 0, cnt = 0;
            for (uint32_t sampleY=0;sampleY<10;++sampleY) {
                uint32_t y=std::min(height-1,y0+(2*sampleY+1)*(y1-y0)/20);
                const uint8_t* row = row0 + static_cast<int64_t>(y) * stride;
                for (uint32_t sampleX=0;sampleX<10;++sampleX) {
                    uint32_t x=std::min(width-1,x0+(2*sampleX+1)*(x1-x0)/20);
                    const uint8_t* px = row + static_cast<int64_t>(x) * 4;
                    accB += px[0];
                    accG += px[1];
                    accR += px[2];
                    ++cnt;
                }
            }
            if (cnt > 0) {
                sig.rgbGrid[idx + 0] = static_cast<uint8_t>(accR / cnt);
                sig.rgbGrid[idx + 1] = static_cast<uint8_t>(accG / cnt);
                sig.rgbGrid[idx + 2] = static_cast<uint8_t>(accB / cnt);
            }
            idx += 3;
        }
    }

    if (width >= 16 && height >= 16) {
        const auto subj = ComputeSubjectFeaturesForBox(row0, width, height, stride, 0.0, 0.0, static_cast<double>(width), static_cast<double>(height));
        sig.subjDHashH = subj.subjDHashH;
        sig.subjDHashV = subj.subjDHashV;
        sig.subjAHash  = subj.subjAHash;
        sig.edgeHashH  = subj.edgeHashH;
        sig.edgeHashV  = subj.edgeHashV;
        sig.edgeAHash  = subj.edgeAHash;
        sig.fgCellMask64 = subj.fgCellMask64;
        memcpy(sig.subjRgbGrid, subj.subjRgbGrid, 48);
        sig.subjFgMask = subj.subjFgMask;
        sig.bgCoherencePermille = subj.bgCoherencePermille;
        sig.hasSubjectDetail = true;

        // If global variance is low AND the subject region has neither edge nor contrast detail, mark lowVariance.
        // Conversely, if a white-on-white garment has clear internal edge/contrast detail in the subject zone,
        // keep lowVariance false so white garments on white backdrops are never discarded as blank frames.
        if (sig.lowVariance && (subj.subjMeanEdge >= 1.25 || subj.subjStdY >= 3.2)) {
            sig.lowVariance = false;
        } else if (!sig.lowVariance && variance < 28.0 && subj.subjMeanEdge < 0.45 && subj.subjStdY < 1.2) {
            sig.lowVariance = true;
        }
    }

    if (!sig.lowVariance && width >= 16 && height >= 16) {
        const double rx0 = width * 0.0375, rx1 = width * 0.9625;
        const double ry0 = height * (7.0 / 180.0), ry1 = height * (173.0 / 180.0);
        const double rw = std::max(4.0, rx1 - rx0), rh = std::max(4.0, ry1 - ry0);
        double clum[9][9]{};
        double csum64 = 0.0;
        for (int gy = 0; gy < 9; ++gy) {
            double cy0 = ry0 + (gy * rh) / 9.0, cy1 = ry0 + ((gy + 1) * rh) / 9.0;
            for (int gx = 0; gx < 9; ++gx) {
                double cx0 = rx0 + (gx * rw) / 9.0, cx1 = rx0 + ((gx + 1) * rw) / 9.0;
                uint64_t accY = 0, count = 0;
                for (uint32_t sampleY = 0; sampleY < 8; ++sampleY) {
                    uint32_t y = std::min(height - 1, static_cast<uint32_t>(cy0 + (2 * sampleY + 1) * (cy1 - cy0) / 16.0));
                    const uint8_t* row = row0 + static_cast<int64_t>(y) * stride;
                    for (uint32_t sampleX = 0; sampleX < 8; ++sampleX) {
                        uint32_t x = std::min(width - 1, static_cast<uint32_t>(cx0 + (2 * sampleX + 1) * (cx1 - cx0) / 16.0));
                        const uint8_t* px = row + static_cast<int64_t>(x) * 4;
                        accY += (77u * px[2] + 150u * px[1] + 29u * px[0]) >> 8;
                        ++count;
                    }
                }
                clum[gy][gx] = count ? (static_cast<double>(accY) / static_cast<double>(count)) : 0.0;
                if (gy < 8 && gx < 8) csum64 += clum[gy][gx];
            }
        }
        double cavg64 = csum64 / 64.0;
        uint64_t ch = 0, cv = 0, ca = 0;
        int cbit = 0;
        for (int gy = 0; gy < 8; ++gy) {
            for (int gx = 0; gx < 8; ++gx) {
                if (clum[gy][gx] > clum[gy][gx + 1] + 2.0) ch |= (1ULL << cbit);
                if (clum[gy][gx] > clum[gy + 1][gx] + 2.0) cv |= (1ULL << cbit);
                if (clum[gy][gx] >= cavg64) ca |= (1ULL << cbit);
                ++cbit;
            }
        }
        sig.centerDHashH = ch;
        sig.centerDHashV = cv;
        sig.centerAHash = ca;
        int cidx = 0;
        for (int gy = 0; gy < 4; ++gy) {
            double cy0 = ry0 + (gy * rh) / 4.0, cy1 = ry0 + ((gy + 1) * rh) / 4.0;
            for (int gx = 0; gx < 4; ++gx) {
                double cx0 = rx0 + (gx * rw) / 4.0, cx1 = rx0 + ((gx + 1) * rw) / 4.0;
                uint64_t accR = 0, accG = 0, accB = 0, cnt = 0;
                for (uint32_t sampleY = 0; sampleY < 10; ++sampleY) {
                    uint32_t y = std::min(height - 1, static_cast<uint32_t>(cy0 + (2 * sampleY + 1) * (cy1 - cy0) / 20.0));
                    const uint8_t* row = row0 + static_cast<int64_t>(y) * stride;
                    for (uint32_t sampleX = 0; sampleX < 10; ++sampleX) {
                        uint32_t x = std::min(width - 1, static_cast<uint32_t>(cx0 + (2 * sampleX + 1) * (cx1 - cx0) / 20.0));
                        const uint8_t* px = row + static_cast<int64_t>(x) * 4;
                        accB += px[0]; accG += px[1]; accR += px[2]; ++cnt;
                    }
                }
                if (cnt > 0) {
                    sig.centerRgbGrid[cidx + 0] = static_cast<uint8_t>(accR / cnt);
                    sig.centerRgbGrid[cidx + 1] = static_cast<uint8_t>(accG / cnt);
                    sig.centerRgbGrid[cidx + 2] = static_cast<uint8_t>(accB / cnt);
                }
                cidx += 3;
            }
        }
        const auto csubj = ComputeSubjectFeaturesForBox(row0, width, height, stride, rx0, ry0, rx1, ry1);
        sig.centerSubjDHashH = csubj.subjDHashH;
        sig.centerSubjDHashV = csubj.subjDHashV;
        sig.centerSubjAHash  = csubj.subjAHash;
        sig.centerEdgeHashH  = csubj.edgeHashH;
        sig.centerEdgeHashV  = csubj.edgeHashV;
        sig.centerEdgeAHash  = csubj.edgeAHash;
        sig.centerFgCellMask64 = csubj.fgCellMask64;
        memcpy(sig.centerSubjRgbGrid, csubj.subjRgbGrid, 48);
        sig.centerSubjFgMask = csubj.subjFgMask;
        sig.centerBgCoherencePermille = csubj.bgCoherencePermille;
        sig.hasCenterCrop = true;
    }
    if(spatialDetail && !sig.lowVariance && width>=16 && height>=16) {
        auto sample=[&](uint8_t* grid,double x0,double y0,double x1,double y1) {
            // Densely sample the subject window without retaining preview pixels.
            const double boxWidth=x1-x0,boxHeight=y1-y0;
            x1-=boxWidth*.25; x0+=boxWidth*.25; y1-=boxHeight*.18; y0+=boxHeight*.18;
            for(int gy=0;gy<12;++gy) for(int gx=0;gx<12;++gx) {
                double sum=0;
                for(int sy=0;sy<4;++sy) for(int sx=0;sx<4;++sx) {
                    double r,g,b,y;
                    SampleBilinearRGBY(row0,width,height,stride,x0+(gx+(sx+.5)/4)*(x1-x0)/12-.5,y0+(gy+(sy+.5)/4)*(y1-y0)/12-.5,r,g,b,y); sum+=y;
                }
                grid[gy*12+gx]=uint8_t(std::clamp(std::lround(sum/16),0L,255L));
            }
        };
        sample(sig.spatialLuma,0,0,width,height);
        sample(sig.centerSpatialLuma,width*.0375,height*(7.0/180),width*.9625,height*(173.0/180));
        sig.hasSpatialDetail=true;
    }
    sig.valid = true;
    return sig;
}

// Generate 96x54 BGRA32 thumbnail with aspect-ratio preservation
void GenerateThumbnailBGRA(const uint8_t* srcData, uint32_t srcW, uint32_t srcH, int32_t srcStride, std::vector<uint8_t>& outThumb) {
    outThumb.assign(THUMB_W * THUMB_H * 4, 0);
    // Fill background with dark charcoal (RGB 24, 24, 28)
    for (int i = 0; i < THUMB_W * THUMB_H; ++i) {
        outThumb[i * 4 + 0] = 28;
        outThumb[i * 4 + 1] = 24;
        outThumb[i * 4 + 2] = 24;
        outThumb[i * 4 + 3] = 255;
    }
    if (!srcData || srcW == 0 || srcH == 0 || std::abs(int64_t(srcStride)) < int64_t(srcW) * 4) return;

    const uint8_t* row0 = srcData;

    double scaleX = static_cast<double>(THUMB_W) / static_cast<double>(srcW);
    double scaleY = static_cast<double>(THUMB_H) / static_cast<double>(srcH);
    double scale  = std::min(scaleX, scaleY);

    int dstW = std::max(1, std::min(THUMB_W, static_cast<int>( std::round(srcW * scale) )));
    int dstH = std::max(1, std::min(THUMB_H, static_cast<int>( std::round(srcH * scale) )));
    int offX = (THUMB_W - dstW) / 2;
    int offY = (THUMB_H - dstH) / 2;

    for (int dy = 0; dy < dstH; ++dy) {
        uint32_t sy0 = (dy * srcH) / dstH;
        uint32_t sy1 = ((dy + 1) * srcH) / dstH;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        uint32_t stepY = std::max(1u, (sy1 - sy0) / 4);

        uint8_t* dstRow = outThumb.data() + ((offY + dy) * THUMB_W + offX) * 4;
        for (int dx = 0; dx < dstW; ++dx) {
            uint32_t sx0 = (dx * srcW) / dstW;
            uint32_t sx1 = ((dx + 1) * srcW) / dstW;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            uint32_t stepX = std::max(1u, (sx1 - sx0) / 4);

            uint32_t accB = 0, accG = 0, accR = 0, cnt = 0;
            for (uint32_t sy = sy0; sy < sy1; sy += stepY) {
                const uint8_t* sRow = row0 + static_cast<int64_t>(sy) * srcStride;
                for (uint32_t sx = sx0; sx < sx1; sx += stepX) {
                    const uint8_t* px = sRow + static_cast<int64_t>(sx) * 4;
                    accB += px[0];
                    accG += px[1];
                    accR += px[2];
                    ++cnt;
                }
            }
            if (cnt > 0) {
                dstRow[dx * 4 + 0] = static_cast<uint8_t>(accB / cnt);
                dstRow[dx * 4 + 1] = static_cast<uint8_t>(accG / cnt);
                dstRow[dx * 4 + 2] = static_cast<uint8_t>(accR / cnt);
                dstRow[dx * 4 + 3] = 255;
            }
        }
    }
}

// Trim only near-black, uniform outer bands; never more than a quarter per edge.
// count limits the orientations: 8 = all, 1 = only the cropped upright frame (normalized[0]).
void AddNormalizedOrientations(const uint8_t* pixels, uint32_t width, uint32_t height, int32_t stride, FrameSignature& result, int count) {
    if(!result.valid || result.lowVariance || width<16 || height<16) return;
    auto dark=[&](int x,int y) { const auto p=pixels+int64_t(y)*stride+x*4; return std::max({p[0],p[1],p[2]})<=20; };
    auto darkRow=[&](int y) { for(int n=0;n<64;++n) if(!dark((2*n+1)*width/128,y)) return false; return true; };
    auto darkColumn=[&](int x) { for(int n=0;n<64;++n) if(!dark(x,(2*n+1)*height/128)) return false; return true; };
    int top=0,bottom=height,left=0,right=width;
    while(top<int(height/4) && darkRow(top)) ++top;
    while(bottom>int(height*3/4) && darkRow(bottom-1)) --bottom;
    while(left<int(width/4) && darkColumn(left)) ++left;
    while(right>int(width*3/4) && darkColumn(right-1)) --right;
    // White padding is removed only as a four-sided, bounded uniform band.
    auto white=[&](int x,int y) { const auto p=pixels+int64_t(y)*stride+x*4; return std::min({p[0],p[1],p[2]})>=250; };
    auto whiteRow=[&](int y) { for(int n=0;n<64;++n) if(!white((2*n+1)*width/128,y)) return false; return true; };
    auto whiteCol=[&](int x) { for(int n=0;n<64;++n) if(!white(x,(2*n+1)*height/128)) return false; return true; };
    if(!top && bottom==int(height) && !left && right==int(width) && whiteRow(0) && whiteRow(height-1) && whiteCol(0) && whiteCol(width-1)) {
        const int limit=int(std::min(width,height)*0.12);
        int band=0;
        while(band<=limit && whiteRow(band) && whiteRow(height-1-band) && whiteCol(band) && whiteCol(width-1-band)) ++band;
        if(band>=2 && band<=limit) { top=left=band; bottom=height-band; right=width-band; }
    }
    // A small canonical box-averaged grid bounds the CPU/memory cost of all eight orientations without point-sampling aliasing.
    constexpr int side=72; std::vector<uint8_t> grid(side*side*4),transformed(grid.size());
    const int spanW = std::max(1, right - left), spanH = std::max(1, bottom - top);
    for(int y=0;y<side;++y) for(int x=0;x<side;++x) {
        uint32_t accB=0,accG=0,accR=0;
        for(int sy=0;sy<3;++sy) {
            int py = std::clamp(top + ((y * 3 + sy) * spanH + spanH / 2) / (side * 3), top, bottom - 1);
            const uint8_t* sRow = pixels + int64_t(py) * stride;
            for(int sx=0;sx<3;++sx) {
                int px = std::clamp(left + ((x * 3 + sx) * spanW + spanW / 2) / (side * 3), left, right - 1);
                const uint8_t* p = sRow + int64_t(px) * 4;
                accB += p[0]; accG += p[1]; accR += p[2];
            }
        }
        uint8_t* dst = &grid[(y*side+x)*4];
        dst[0] = static_cast<uint8_t>((accB + 4) / 9);
        dst[1] = static_cast<uint8_t>((accG + 4) / 9);
        dst[2] = static_cast<uint8_t>((accR + 4) / 9);
        dst[3] = 255;
    }
    result.normalized.clear();
    for(int mirror=0;mirror<2;++mirror) for(int turn=0;turn<4;++turn) {
        if(int(result.normalized.size())>=count) return;
        for(int y=0;y<side;++y) for(int x=0;x<side;++x) {
            int dx=mirror?side-1-x:x,dy=y;
            for(int rotation=0;rotation<turn;++rotation) { int old=dx; dx=side-1-dy; dy=old; }
            memcpy(&transformed[(dy*side+dx)*4],&grid[(y*side+x)*4],4);
        }
        auto signature=ComputeSignatureFromBGRA(transformed.data(),side,side,side*4,result.hasSpatialDetail);
        signature.timestamp100ns=result.timestamp100ns; result.normalized.push_back(std::move(signature));
    }
}

double CompareImageSignaturesOneWay(const FrameSignature& a, const FrameSignature& b, double scoreFloor) {
    double coarse = CompareSignatures(a, b);
    if (!a.valid || !b.valid || a.lowVariance || b.lowVariance || !a.hasSubjectDetail || !b.hasSubjectDetail) {
        return coarse;
    }

    // One shared affine exposure mapping preserves hue; never normalize channels independently.
    double meanA=0,meanB=0;
    for(int i=0;i<48;++i) { meanA+=a.rgbGrid[i]/48.0; meanB+=b.rgbGrid[i]/48.0; }
    double variance=0,covariance=0;
    for(int i=0;i<48;++i) { variance+=(a.rgbGrid[i]-meanA)*(a.rgbGrid[i]-meanA); covariance+=(a.rgbGrid[i]-meanA)*(b.rgbGrid[i]-meanB); }
    const double gain=variance>1?covariance/variance:1.0, bias=meanB-gain*meanA;
    double exposureError=0;
    for(int i=0;i<48;++i) exposureError+=std::abs(std::clamp(gain*a.rgbGrid[i]+bias,0.0,255.0)-b.rgbGrid[i])/48.0;
    const bool exposure=variance>100 && gain>=0.65 && gain<=1.55 && std::abs(bias)<=55 && exposureError<=8;
    if(exposure) {
        const auto bits=std::popcount(a.dHashH^b.dHashH)+std::popcount(a.dHashV^b.dHashV)+std::popcount(a.aHash^b.aHash);
        coarse=std::max(coarse,65.0*(1.0-bits/192.0)+35.0*std::max(0.0,1.0-exposureError/85.0));
    }
    uint64_t active64 = a.fgCellMask64 | b.fgCellMask64;
    if (std::popcount(active64) < 8) active64 = ~0ULL;
    const double nActive = static_cast<double>(std::popcount(active64));

    const int dSH = std::popcount((a.subjDHashH ^ b.subjDHashH) & active64);
    const int dSV = std::popcount((a.subjDHashV ^ b.subjDHashV) & active64);
    const int dSA = std::popcount((a.subjAHash  ^ b.subjAHash)  & active64);
    const double fgStructSim = 100.0 * (1.0 - static_cast<double>(dSH + dSV + dSA) / (3.0 * nActive));

    const int dEH = std::popcount((a.edgeHashH ^ b.edgeHashH) & active64);
    const int dEV = std::popcount((a.edgeHashV ^ b.edgeHashV) & active64);
    const int dEA = std::popcount((a.edgeAHash ^ b.edgeAHash) & active64);
    const double fgEdgeSim = 100.0 * (1.0 - static_cast<double>(dEH + dEV + dEA) / (3.0 * nActive));

    uint16_t active16 = a.subjFgMask | b.subjFgMask;
    if (active16 == 0) active16 = 0xFFFFu;
    double colorDiffSum = 0,maxCellDiff = 0; int activeChannels = 0;
    for (int c = 0; c < 16; ++c) {
        if (active16 & (1u << c)) {
            double cellDiff=0;
            for(int channel=0;channel<3;++channel) {
                const auto i=c*3+channel;
                const double mapped=exposure?std::clamp(gain*a.subjRgbGrid[i]+bias,0.0,255.0):a.subjRgbGrid[i];
                cellDiff+=std::abs(mapped-b.subjRgbGrid[i]);
            }
            colorDiffSum += cellDiff;
            maxCellDiff = std::max(maxCellDiff, cellDiff / 3);
            activeChannels += 3;
        }
    }
    const double avgSubjColorDiff = activeChannels > 0 ? static_cast<double>(colorDiffSum) / activeChannels : 0.0;
    const double effColorDiff = 0.70 * avgSubjColorDiff + 0.30 * std::max(avgSubjColorDiff, static_cast<double>(maxCellDiff) * 0.65);
    const double fgColorSim = std::max(0.0, 100.0 * (1.0 - effColorDiff / 75.0));

    const double detailSim = 0.58 * fgStructSim + 0.42 * fgEdgeSim;
    double fgCombined = 0.44 * fgStructSim + 0.26 * fgEdgeSim + 0.30 * fgColorSim;
    if (avgSubjColorDiff < 14.0) {
        // When both garments share nearly identical color (e.g. white-on-white or dark-on-dark),
        // require structural cut and local edge layout to support the match.
        fgCombined = std::min(fgCombined, 0.84 * detailSim + 0.16 * fgColorSim);
    }

    const bool bothCoherentBg = (a.bgCoherencePermille >= 550 && b.bgCoherencePermille >= 550);
    if (bothCoherentBg) {
        const int fgInter = std::popcount(a.fgCellMask64 & b.fgCellMask64);
        const int fgUnion = std::max(1, std::popcount(a.fgCellMask64 | b.fgCellMask64));
        const double fgIoU = static_cast<double>(fgInter) / static_cast<double>(fgUnion);
        if (fgIoU < 0.72 && !(exposure && detailSim>=90.0)) {
            fgCombined *= (0.75 + 0.25 * (fgIoU / 0.72));
        }
        const double stage2 = 0.15 * coarse + 0.85 * fgCombined;
        const double score=std::min(coarse,stage2);
        return score<scoreFloor?score:std::min(score,CompareSpatialDetail(a,b,exposure?gain:1.0));
    }

    const double generalStage2 = 0.60 * coarse + 0.40 * fgCombined;
    const double score=std::min(coarse,generalStage2);
    return score<scoreFloor?score:std::min(score,CompareSpatialDetail(a,b,exposure?gain:1.0));
}

double CompareImageFiles(
    const VideoFileInfo& a,
    const VideoFileInfo& b,
    bool matchTransforms,
    double minFrameSimilarity,
    bool* outAccepted,
    bool decisionOnly)
{
    if (outAccepted) *outAccepted = false;
    if (!a.frameLoaded || !b.frameLoaded || !a.sigFirst.valid || !b.sigFirst.valid) {
        return 0.0;
    }

    const bool aAnim = (a.imageKind == ImageEvidenceKind::AnimatedImage);
    const bool bAnim = (b.imageKind == ImageEvidenceKind::AnimatedImage);
    if (aAnim != bAnim) {
        return 0.0;
    }

    const bool aMultiPage = (a.imageKind == ImageEvidenceKind::MultiPageDocument);
    const bool bMultiPage = (b.imageKind == ImageEvidenceKind::MultiPageDocument);
    if (aMultiPage != bMultiPage) {
        return 0.0;
    }
    if (aMultiPage && bMultiPage && a.imageFrameCount != b.imageFrameCount) {
        return 0.0;
    }

    if ((a.imageKind == ImageEvidenceKind::RawEmbeddedPreview && b.imageKind == ImageEvidenceKind::RawDeveloped) ||
        (a.imageKind == ImageEvidenceKind::RawDeveloped && b.imageKind == ImageEvidenceKind::RawEmbeddedPreview)) {
        return 0.0;
    }

    if (aAnim || aMultiPage) {
        if (aAnim && a.duration100ns > 0 && b.duration100ns > 0) {
            const int64_t maxDur = std::max(a.duration100ns, b.duration100ns);
            const int64_t diffDur = std::abs(a.duration100ns - b.duration100ns);
            if (diffDur > std::max<int64_t>(5000000LL, (maxDur * 35) / 100)) {
                return 0.0;
            }
        }
        if (a.samples.size() < 2 || b.samples.size() < 2 || a.samples.size() != b.samples.size()) {
            return 0.0;
        }
        double bestAccepted = 0.0;
        double bestRejected = 0.0;
        bool accepted = false;
        const size_t sampleCount = a.samples.size();
        for (int orientation = -1; orientation < (matchTransforms ? 8 : 0); ++orientation) {
            unsigned valid = 0, matches = 0, nearMatches = 0;
            double total = 0.0;
            double minSampleSim = 100.0;
            for (size_t i = 0; i < sampleCount; ++i) {
                const auto& x = a.samples[i];
                const auto& y = b.samples[i];
                if (!x.valid || !y.valid) break;
                if (orientation >= 0 && (x.normalized.size() != 8 || y.normalized.size() != 8)) break;
                const auto& left = (orientation < 0) ? x : x.normalized[0];
                const auto& right = (orientation < 0) ? y : y.normalized[orientation];
                if (!left.valid || !right.valid || left.lowVariance || right.lowVariance) break;
                const double sim = matchTransforms
                    ? CompareImageSignaturesWithCrop(left, right)
                    : CompareImageSignatures(left, right);
                total += sim;
                minSampleSim = std::min(minSampleSim, sim);
                ++valid;
                if (sim >= minFrameSimilarity) ++matches;
                if (sim >= minFrameSimilarity - 3.0) ++nearMatches;
            }
            if (valid == sampleCount) {
                const double avgSim = total / static_cast<double>(valid);
                const bool orientOk = (minSampleSim >= minFrameSimilarity - 3.5) &&
                    (nearMatches == valid) &&
                    (matches * 5 >= valid * 4) &&
                    (avgSim >= minFrameSimilarity);
                if (orientOk) {
                    accepted = true;
                    bestAccepted = std::max(bestAccepted, avgSim);
                } else {
                    bestRejected = std::max(bestRejected, std::min(avgSim, minSampleSim));
                }
            }
        }
        if (outAccepted) *outAccepted = accepted;
        return accepted ? bestAccepted : bestRejected;
    }

    if(a.sigFirst.lowVariance || b.sigFirst.lowVariance) return 0.0;
    // A rejected upper bound is sufficient when the caller only needs a decision.
    const double scoreFloor=decisionOnly?minFrameSimilarity:0;
    double best = matchTransforms
        ? CompareImageSignaturesWithCrop(a.sigFirst, b.sigFirst,scoreFloor)
        : CompareImageSignatures(a.sigFirst, b.sigFirst,scoreFloor);
    if (matchTransforms && a.sigFirst.normalized.size() == 8 && b.sigFirst.normalized.size() == 8) {
        for (const auto& candidate : b.sigFirst.normalized) {
            if (!a.sigFirst.normalized[0].lowVariance && candidate.valid && !candidate.lowVariance) {
                best = std::max(best, CompareImageSignaturesWithCrop(a.sigFirst.normalized[0], candidate,std::max(scoreFloor,best)));
            }
        }
        for(const auto& candidate:a.sigFirst.normalized) if(!b.sigFirst.normalized[0].lowVariance && candidate.valid && !candidate.lowVariance)
            best=std::max(best,CompareImageSignaturesWithCrop(candidate,b.sigFirst.normalized[0],std::max(scoreFloor,best)));
    }
    if (outAccepted) *outAccepted = (best >= minFrameSimilarity);
    return best;
}

} // namespace PerceptualHash
