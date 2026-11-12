#include "VideoScanner.h"
#include "platform/Platform.h"
#include <algorithm>
#include <cmath>

double VideoScanner::ComputePairSimilarity(
    const VideoFileInfo& a,
    const VideoFileInfo& b,
    bool matchTransforms)
{
    if (a.corrupted || b.corrupted) return 0.0;
    if (!a.contentHash.empty() && a.contentHash == b.contentHash && a.fileSize == b.fileSize) {
        return 100.0;
    }
    if (a.previewFailed || b.previewFailed) return 0.0;
    if (IsImageExtension(a.fullPath.empty() ? a.fileName : a.fullPath)) {
        if (!a.frameLoaded || !b.frameLoaded) return 0.0;
        return PerceptualHash::CompareImageFiles(a, b, matchTransforms, 0.0);
    }
    if (a.samples.size() == 5 && b.samples.size() == 5) {
        double best = 0.0;
        bool hasValid = false;
        for (int orientation = -1; orientation < (matchTransforms ? 8 : 0); ++orientation) {
            unsigned valid = 0;
            double total = 0.0;
            for (size_t i = 0; i < 5; ++i) {
                const auto& x = a.samples[i];
                const auto& y = b.samples[i];
                if (!x.valid || !y.valid) continue;
                if (orientation >= 0 && (x.normalized.size() != 8 || y.normalized.size() != 8)) continue;
                const auto& left = orientation < 0 ? x : x.normalized[0];
                const auto& right = orientation < 0 ? y : y.normalized[orientation];
                if (!left.valid || !right.valid || left.lowVariance || right.lowVariance) continue;
                const double sim = matchTransforms
                    ? PerceptualHash::CompareSignaturesWithCrop(left, right)
                    : PerceptualHash::CompareSignatures(left, right);
                total += sim;
                ++valid;
            }
            if (valid >= 3) {
                best = std::max(best, total / valid);
                hasValid = true;
            }
        }
        if (hasValid) return best;
    }
    if (a.frameLoaded && b.frameLoaded) {
        return PerceptualHash::ComputeVideoFrameSimilarity(a, b);
    }
    return 0.0;
}

void VideoScanner::SortGroups(
    const std::vector<VideoFileInfo>& files,
    std::vector<VideoGroup>& groups,
    SortOrder order, bool matchTransforms)
{
    auto fileComparator = [&](int idxA, int idxB) -> bool {
        const auto& a = files[idxA];
        const auto& b = files[idxB];
        if (a.protectedFile != b.protectedFile) return a.protectedFile;
        switch (order) {
            case SortOrder::SizeDesc:
            case SortOrder::SimilarityDesc:
            case SortOrder::SimilarityAsc:
                if (a.fileSize != b.fileSize) return a.fileSize > b.fileSize;
                if (a.duration100ns != b.duration100ns) return a.duration100ns > b.duration100ns;
                return a.fullPath < b.fullPath;
            case SortOrder::SizeAsc:
                if (a.fileSize != b.fileSize) return a.fileSize < b.fileSize;
                if (a.duration100ns != b.duration100ns) return a.duration100ns < b.duration100ns;
                return a.fullPath < b.fullPath;
            case SortOrder::DurationDesc:
                if (a.duration100ns != b.duration100ns) return a.duration100ns > b.duration100ns;
                if (a.fileSize != b.fileSize) return a.fileSize > b.fileSize;
                return a.fullPath < b.fullPath;
            case SortOrder::DurationAsc:
                if (a.duration100ns != b.duration100ns) return a.duration100ns < b.duration100ns;
                if (a.fileSize != b.fileSize) return a.fileSize < b.fileSize;
                return a.fullPath < b.fullPath;
            case SortOrder::ResolutionDesc:
                if (a.PixelCount() != b.PixelCount()) return a.PixelCount() > b.PixelCount();
                if (a.fileSize != b.fileSize) return a.fileSize > b.fileSize;
                return a.fullPath < b.fullPath;
            case SortOrder::ResolutionAsc:
                if (a.PixelCount() != b.PixelCount()) return a.PixelCount() < b.PixelCount();
                if (a.fileSize != b.fileSize) return a.fileSize < b.fileSize;
                return a.fullPath < b.fullPath;
            case SortOrder::NameAsc:
            default:
                if (a.fileName != b.fileName) {
                    return Platform::CompareFileNamesNatural(a.fileName, b.fileName) < 0;
                }
                return a.fullPath < b.fullPath;
        }
    };

    auto simRank = [](double s) -> int64_t {
        return std::isfinite(s) ? std::llround(std::clamp(s, 0.0, 100.0) * 10000.0) : -1;
    };

    for (auto& g : groups) {
        // A reference chosen by hand stays in slot 0; only the candidates are ordered.
        std::sort(g.fileIndices.begin() + (g.manualReference && !g.fileIndices.empty() ? 1 : 0), g.fileIndices.end(), fileComparator);
        if (g.fileIndices.size() > 2 && (order == SortOrder::SimilarityDesc || order == SortOrder::SimilarityAsc)) {
            const int refIdx = g.fileIndices[0];
            struct MemberScore {
                int idx;
                bool exact;
                int64_t rank;
            };
            std::vector<MemberScore> scored;
            scored.reserve(g.fileIndices.size() - 1);
            for (size_t k = 1; k < g.fileIndices.size(); ++k) {
                const int idx = g.fileIndices[k];
                const bool exact = !files[refIdx].contentHash.empty() &&
                    files[refIdx].contentHash == files[idx].contentHash &&
                    files[refIdx].fileSize == files[idx].fileSize;
                const int64_t rank = exact ? 1000000 : simRank(ComputePairSimilarity(files[refIdx], files[idx], matchTransforms));
                scored.push_back({idx, exact, rank});
            }
            std::sort(scored.begin(), scored.end(), [&](const MemberScore& a, const MemberScore& b) {
                if (a.exact != b.exact) return order == SortOrder::SimilarityDesc ? a.exact : !a.exact;
                if (a.rank != b.rank) return order == SortOrder::SimilarityDesc ? (a.rank > b.rank) : (a.rank < b.rank);
                return fileComparator(a.idx, b.idx);
            });
            for (size_t k = 0; k < scored.size(); ++k) {
                g.fileIndices[k + 1] = scored[k].idx;
            }
        }
        g.leftSlot = 0;
        g.rightSlot = (g.fileIndices.size() > 1) ? 1 : 0;
        g.bestFrameSim = (g.fileIndices.size() >= 2)
            ? ComputePairSimilarity(files[g.fileIndices[0]], files[g.fileIndices[g.rightSlot]], matchTransforms)
            : 0.0;
    }

    auto isExactGroup = [&](const VideoGroup& g) -> bool {
        if (g.fileIndices.size() < 2) return false;
        const auto& first = files[g.fileIndices[0]];
        if (first.deleted || first.corrupted || !first.identity.valid || first.contentHash.empty()) return false;
        for (size_t i = 1; i < g.fileIndices.size(); ++i) {
            const auto& f = files[g.fileIndices[i]];
            if (f.deleted || f.corrupted || !f.identity.valid || f.fileSize != first.fileSize || f.contentHash != first.contentHash) {
                return false;
            }
        }
        return true;
    };

    std::sort(groups.begin(), groups.end(), [&](const VideoGroup& ga, const VideoGroup& gb) {
        if (ga.fileIndices.empty() || gb.fileIndices.empty()) {
            return ga.fileIndices.size() > gb.fileIndices.size();
        }
        if (order == SortOrder::SimilarityDesc || order == SortOrder::SimilarityAsc) {
            const bool exactA = isExactGroup(ga);
            const bool exactB = isExactGroup(gb);
            if (exactA != exactB) return order == SortOrder::SimilarityDesc ? exactA : !exactA;
            const int64_t rankA = simRank(ga.bestFrameSim);
            const int64_t rankB = simRank(gb.bestFrameSim);
            if (rankA != rankB) {
                return order == SortOrder::SimilarityDesc
                    ? (rankA > rankB)
                    : (rankA < rankB);
            }
        }
        return fileComparator(ga.fileIndices[0], gb.fileIndices[0]);
    });
}
