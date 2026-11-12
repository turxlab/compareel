#pragma once
#include "Common.h"
#include <algorithm>
#include <bit>
#include <cmath>

namespace PerceptualHash {

struct SubjectRegionFeatures {
    uint64_t subjDHashH = 0;
    uint64_t subjDHashV = 0;
    uint64_t subjAHash  = 0;
    uint64_t edgeHashH  = 0;
    uint64_t edgeHashV  = 0;
    uint64_t edgeAHash  = 0;
    uint64_t fgCellMask64 = 0;
    uint8_t  subjRgbGrid[16 * 3]{};
    uint16_t subjFgMask = 0;
    uint16_t bgCoherencePermille = 0;
    double   subjStdY = 0.0;
    double   subjMeanEdge = 0.0;
};

inline void SampleBilinearRGBY(
    const uint8_t* row0, uint32_t width, uint32_t height, int32_t stride,
    double fx, double fy, double& outR, double& outG, double& outB, double& outY)
{
    fx = std::clamp(fx, 0.0, static_cast<double>(width - 1));
    fy = std::clamp(fy, 0.0, static_cast<double>(height - 1));
    uint32_t x0 = static_cast<uint32_t>(fx);
    uint32_t y0 = static_cast<uint32_t>(fy);
    uint32_t x1 = std::min(width - 1, x0 + 1);
    uint32_t y1 = std::min(height - 1, y0 + 1);
    double wx = fx - x0;
    double wy = fy - y0;
    const uint8_t* r0 = row0 + static_cast<int64_t>(y0) * stride;
    const uint8_t* r1 = row0 + static_cast<int64_t>(y1) * stride;
    const uint8_t* p00 = r0 + static_cast<int64_t>(x0) * 4;
    const uint8_t* p01 = r0 + static_cast<int64_t>(x1) * 4;
    const uint8_t* p10 = r1 + static_cast<int64_t>(x0) * 4;
    const uint8_t* p11 = r1 + static_cast<int64_t>(x1) * 4;
    double w00 = (1.0 - wx) * (1.0 - wy);
    double w01 = wx * (1.0 - wy);
    double w10 = (1.0 - wx) * wy;
    double w11 = wx * wy;
    outB = w00 * p00[0] + w01 * p01[0] + w10 * p10[0] + w11 * p11[0];
    outG = w00 * p00[1] + w01 * p01[1] + w10 * p10[1] + w11 * p11[1];
    outR = w00 * p00[2] + w01 * p01[2] + w10 * p10[2] + w11 * p11[2];
    outY = (77.0 * outR + 150.0 * outG + 29.0 * outB) / 256.0;
}

SubjectRegionFeatures ComputeSubjectFeaturesForBox(
    const uint8_t* row0, uint32_t width, uint32_t height, int32_t stride,
    double boxX0, double boxY0, double boxX1, double boxY1);

// Compute FrameSignature from a BGRA32 buffer (width, height, stride in bytes)
FrameSignature ComputeSignatureFromBGRA(const uint8_t* data, uint32_t width, uint32_t height, int32_t stride, bool spatialDetail = false);

// Generate 96x54 BGRA32 thumbnail with aspect-ratio preservation
void GenerateThumbnailBGRA(const uint8_t* srcData, uint32_t srcW, uint32_t srcH, int32_t srcStride, std::vector<uint8_t>& outThumb);

// Trim only near-black, uniform outer bands; never more than a quarter per edge.
// count limits the orientations: 8 = all, 1 = only the cropped upright frame (normalized[0]).
void AddNormalizedOrientations(const uint8_t* pixels, uint32_t width, uint32_t height, int32_t stride, FrameSignature& result, int count = 8);

// Compare two single FrameSignatures (returns 0.0 to 100.0)
inline double CompareSignatures(const FrameSignature& a, const FrameSignature& b) {
    if (!a.valid || !b.valid) return 0.0;

    // Color similarity (4x4 RGB grid = 48 channels, max diff per channel = 255)
    int totalColorDiff = 0;
    for (int i = 0; i < 48; ++i) {
        totalColorDiff += std::abs(static_cast<int>(a.rgbGrid[i]) - static_cast<int>(b.rgbGrid[i]));
    }
    // Normalize color similarity: average channel diff of 0 -> 100%, diff of 64 -> 0%
    double avgChannelDiff = static_cast<double>(totalColorDiff) / 48.0;
    double colorSim = std::max(0.0, 100.0 * (1.0 - (avgChannelDiff / 85.0)));

    // If both frames are low-variance (e.g. solid color), rely primarily on exact RGB match
    if (a.lowVariance && b.lowVariance) {
        return std::max(0.0, 100.0 * (1.0 - (avgChannelDiff / 25.0)));
    }
    if (a.lowVariance != b.lowVariance && avgChannelDiff > 15.0) {
        return colorSim * 0.5;
    }

    // Structural Hamming distance (192 bits total).
    int diffH = std::popcount(a.dHashH ^ b.dHashH);
    int diffV = std::popcount(a.dHashV ^ b.dHashV);
    int diffA = std::popcount(a.aHash  ^ b.aHash);

    double structSim = 100.0 * (1.0 - static_cast<double>(diffH + diffV + diffA) / 192.0);

    // Weighted combination: 65% structural gradient/luma hash + 35% spatial RGB color similarity
    return 0.65 * structSim + 0.35 * colorSim;
}

inline double CompareSignaturesWithCrop(const FrameSignature& a, const FrameSignature& b) {
    double best = CompareSignatures(a, b);
    if (!a.valid || !b.valid || a.lowVariance || b.lowVariance) return best;
    if (a.hasCenterCrop) {
        FrameSignature ca{};
        ca.valid = true; ca.lowVariance = false;
        ca.dHashH = a.centerDHashH; ca.dHashV = a.centerDHashV; ca.aHash = a.centerAHash;
        memcpy(ca.rgbGrid, a.centerRgbGrid, 48);
        best = std::max(best, CompareSignatures(ca, b));
    }
    if (b.hasCenterCrop) {
        FrameSignature cb{};
        cb.valid = true; cb.lowVariance = false;
        cb.dHashH = b.centerDHashH; cb.dHashV = b.centerDHashV; cb.aHash = b.centerAHash;
        memcpy(cb.rgbGrid, b.centerRgbGrid, 48);
        best = std::max(best, CompareSignatures(a, cb));
    }
    return best;
}

// Stage-2 image comparison with foreground/subject ROI & local-detail verification (IMG5).
// Retains the 65% structural bound even when exposure color differences are normalized.
inline double CompareSpatialDetail(const FrameSignature& a,const FrameSignature& b,double gain) {
    if(!a.hasSpatialDetail || !b.hasSpatialDetail) return 100;
    double scores[16]{};
    for(int by=0;by<4;++by) for(int bx=0;bx<4;++bx) {
        // Small local shifts tolerate resampling and mild crop differences.
        for(double dy:{-.5,0.,.5}) for(double dx:{-.5,0.,.5}) {
            double x=0,y=0,xx=0,yy=0,xy=0;
            for(int py=0;py<3;++py) for(int px=0;px<3;++px) {
                const int gx=bx*3+px,gy=by*3+py,i=gy*12+gx;
                const double fx=std::clamp(gx+dx,0.,11.),fy=std::clamp(gy+dy,0.,11.);
                const int x0=int(fx),y0=int(fy),x1=std::min(11,x0+1),y1=std::min(11,y0+1);
                const double wx=fx-x0,wy=fy-y0;
                const double top=(1-wx)*b.spatialLuma[y0*12+x0]+wx*b.spatialLuma[y0*12+x1];
                const double bottom=(1-wx)*b.spatialLuma[y1*12+x0]+wx*b.spatialLuma[y1*12+x1];
                const double u=a.spatialLuma[i]*gain,v=(1-wy)*top+wy*bottom;
                x+=u; y+=v; xx+=u*u; yy+=v*v; xy+=u*v;
            }
            const double vx=std::max(0.0,xx/9-x*x/81),vy=std::max(0.0,yy/9-y*y/81),cov=xy/9-x*y/81;
            scores[by*4+bx]=std::max(scores[by*4+bx],std::clamp((2*cov+100)/(vx+vy+100),0.0,1.0));
        }
    }
    // The worst local regions expose changed subjects hidden by a shared background.
    std::sort(std::begin(scores),std::end(scores));
    return 100*(scores[0]+scores[1]+scores[2])/3;
}

double CompareImageSignaturesOneWay(const FrameSignature& a, const FrameSignature& b, double scoreFloor = 0);

inline double StructuralScoreUpperBound(uint64_t ah, uint64_t av, uint64_t aa, uint64_t bh, uint64_t bv, uint64_t ba) {
    const int bits = std::popcount(ah ^ bh) + std::popcount(av ^ bv) + std::popcount(aa ^ ba);
    return 100.0 - 65.0 * (static_cast<double>(bits) / 192.0);
}

inline double CompareImageSignatures(const FrameSignature& a,const FrameSignature& b,double scoreFloor=0) {
    if(scoreFloor>0.0 && a.valid && b.valid && !a.lowVariance && !b.lowVariance) {
        const double ub=StructuralScoreUpperBound(a.dHashH,a.dHashV,a.aHash,b.dHashH,b.dHashV,b.aHash);
        if(ub+1e-5<scoreFloor) return ub;
        const double first=CompareImageSignaturesOneWay(a,b,scoreFloor);
        const double secondFloor=std::max(scoreFloor,first);
        if(ub+1e-5<secondFloor) return first;
        return std::max(first,CompareImageSignaturesOneWay(b,a,secondFloor));
    }
    const double first=CompareImageSignaturesOneWay(a,b,scoreFloor);
    return std::max(first,CompareImageSignaturesOneWay(b,a,std::max(scoreFloor,first)));
}

inline FrameSignature MakeCenterCropSignature(const FrameSignature& src) {
    FrameSignature c{};
    c.valid = true;
    c.lowVariance = false;
    c.dHashH = src.centerDHashH;
    c.dHashV = src.centerDHashV;
    c.aHash  = src.centerAHash;
    memcpy(c.rgbGrid, src.centerRgbGrid, 48);
    c.hasSubjectDetail = src.hasSubjectDetail;
    c.subjDHashH = src.centerSubjDHashH;
    c.subjDHashV = src.centerSubjDHashV;
    c.subjAHash  = src.centerSubjAHash;
    c.edgeHashH  = src.centerEdgeHashH;
    c.edgeHashV  = src.centerEdgeHashV;
    c.edgeAHash  = src.centerEdgeAHash;
    c.fgCellMask64 = src.centerFgCellMask64;
    memcpy(c.subjRgbGrid, src.centerSubjRgbGrid, 48);
    c.subjFgMask = src.centerSubjFgMask;
    c.bgCoherencePermille = src.centerBgCoherencePermille;
    c.hasSpatialDetail=src.hasSpatialDetail;
    memcpy(c.spatialLuma,src.centerSpatialLuma,144);
    return c;
}

inline double CompareImageSignaturesWithCrop(const FrameSignature& a, const FrameSignature& b,double scoreFloor=0) {
    double best = CompareImageSignatures(a, b,scoreFloor);
    if (!a.valid || !b.valid || a.lowVariance || b.lowVariance) return best;
    if (a.hasCenterCrop) {
        const double cropFloor = std::max(scoreFloor, best);
        if (cropFloor <= 0.0 || StructuralScoreUpperBound(a.centerDHashH, a.centerDHashV, a.centerAHash, b.dHashH, b.dHashV, b.aHash) + 1e-5 >= cropFloor) {
            best = std::max(best, CompareImageSignatures(MakeCenterCropSignature(a), b, cropFloor));
        }
    }
    if (b.hasCenterCrop) {
        const double cropFloor = std::max(scoreFloor, best);
        if (cropFloor <= 0.0 || StructuralScoreUpperBound(a.dHashH, a.dHashV, a.aHash, b.centerDHashH, b.centerDHashV, b.centerAHash) + 1e-5 >= cropFloor) {
            best = std::max(best, CompareImageSignatures(a, MakeCenterCropSignature(b), cropFloor));
        }
    }
    return best;
}

// Compare two VideoFileInfo first frames (checks primary t=0 frame and fallback t=0.5s frame if t=0 is blank)
inline double ComputeVideoFrameSimilarity(const VideoFileInfo& a, const VideoFileInfo& b) {
    if (!a.frameLoaded || !b.frameLoaded) return 0.0;

    if (a.sigFirst.lowVariance || b.sigFirst.lowVariance) {
        const auto& sa = a.sigFirst.lowVariance ? a.sigAlt : a.sigFirst;
        const auto& sb = b.sigFirst.lowVariance ? b.sigAlt : b.sigFirst;
        if (!sa.valid || !sb.valid || sa.lowVariance || sb.lowVariance) return 0.0;
        return CompareSignatures(sa, sb);
    }
    // Both first frames carry detail here; blank fade-ins were handled above.
    double best = CompareSignatures(a.sigFirst, b.sigFirst);
    if (a.sigAlt.valid && b.sigAlt.valid) best = std::max(best, CompareSignatures(a.sigAlt, b.sigAlt));
    return best;
}

double CompareImageFiles(
    const VideoFileInfo& a,
    const VideoFileInfo& b,
    bool matchTransforms,
    double minFrameSimilarity = 90.0,
    bool* outAccepted = nullptr,
    bool decisionOnly = false);

} // namespace PerceptualHash
