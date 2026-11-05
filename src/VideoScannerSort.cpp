#include "VideoScanner.h"
#include "platform/Platform.h"

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

    for (auto& g : groups) {
        // A reference chosen by hand stays in slot 0; only the candidates are ordered.
        std::sort(g.fileIndices.begin() + (g.manualReference && !g.fileIndices.empty() ? 1 : 0), g.fileIndices.end(), fileComparator);
        g.leftSlot = 0;
        g.rightSlot = (g.fileIndices.size() > 1) ? 1 : 0;
        if (g.fileIndices.size() >= 2) {
            const auto& fa = files[g.fileIndices[0]];
            const auto& fb = files[g.fileIndices[1]];
            g.bestFrameSim = IsImageExtension(fa.fileName)
                ? PerceptualHash::CompareImageFiles(fa, fb, matchTransforms)
                : PerceptualHash::ComputeVideoFrameSimilarity(fa, fb);
        }
    }

    std::sort(groups.begin(), groups.end(), [&](const VideoGroup& ga, const VideoGroup& gb) {
        if (ga.fileIndices.empty() || gb.fileIndices.empty()) {
            return ga.fileIndices.size() > gb.fileIndices.size();
        }
        return fileComparator(ga.fileIndices[0], gb.fileIndices[0]);
    });
}
