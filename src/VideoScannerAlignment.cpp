#include "VideoScanner.h"
#include "FFmpegReader.h"

PartialClipAlignment VideoScanner::EstimatePartialClipAlignment(
    const std::wstring& leftPath,
    const std::wstring& rightPath,
    std::atomic<bool>* cancel,
    double minSimilarity)
{
    PartialClipAlignment result{};
    if (leftPath.empty() || rightPath.empty() || (cancel && cancel->load())) return result;
    FFmpegReader leftReader, rightReader;
    VideoFileInfo leftInfo, rightInfo;
    leftInfo.fullPath = leftPath;
    rightInfo.fullPath = rightPath;
    if (!leftReader.Open(leftPath, cancel) || !leftReader.Metadata(leftInfo) ||
        !rightReader.Open(rightPath, cancel) || !rightReader.Metadata(rightInfo)) {
        return result;
    }
    if (leftInfo.duration100ns < 15000000LL || rightInfo.duration100ns < 15000000LL) {
        return result;
    }
    const int64_t maxDur = std::max(leftInfo.duration100ns, rightInfo.duration100ns);
    const int64_t step100ns = maxDur > 600000000LL
        ? std::max<int64_t>(5000000LL, ((maxDur / 100) / 1000000LL) * 1000000LL)
        : 5000000LL;
    std::vector<FrameSignature> leftSamples, rightSamples;
    // Alignment compares upright frames only (pairSim reads normalized[0]); the other seven orientations would be wasted work.
    if (!leftReader.SampleTimeline(step100ns, 120, leftSamples, 1) ||
        !rightReader.SampleTimeline(step100ns, 120, rightSamples, 1) ||
        leftSamples.size() < 4 || rightSamples.size() < 4) {
        return result;
    }

    auto pairSim = [](const FrameSignature& a, const FrameSignature& b) -> double {
        if (!a.valid || !b.valid || a.lowVariance || b.lowVariance) return 0.0;
        double s = PerceptualHash::CompareSignaturesWithCrop(a, b);
        if (!a.normalized.empty() && !b.normalized.empty() &&
            a.normalized[0].valid && b.normalized[0].valid &&
            !a.normalized[0].lowVariance && !b.normalized[0].lowVariance) {
            s = std::max(s, PerceptualHash::CompareSignaturesWithCrop(a.normalized[0], b.normalized[0]));
        }
        return s;
    };

    struct ShiftCandidate {
        int shift = 0;
        size_t startIdx = 0;
        size_t endIdx = 0;
        unsigned matched = 0;
        unsigned validCount = 0;
        double meanSim = 0.0;
        double quality = 0.0;
        int64_t sumOffset100ns = 0;
    };

    const int nL = static_cast<int>(leftSamples.size());
    const int nR = static_cast<int>(rightSamples.size());
    std::vector<ShiftCandidate> candidates;

    for (int d = -(nL - 4); d <= (nR - 4); ++d) {
        if (cancel && cancel->load()) return {};
        const int iBegin = std::max(0, -d);
        const int iEnd = std::min(nL, nR - d);
        if (iEnd - iBegin < 4) continue;

        ShiftCandidate bestForShift{};
        bestForShift.shift = d;
        size_t runStart = static_cast<size_t>(iBegin);
        unsigned runMatched = 0, runValid = 0;
        double runSumSim = 0.0;
        int64_t runSumOffset = 0;

        auto commitRun = [&](size_t rStart, size_t rEnd, unsigned rMatched, unsigned rValid, double rSumSim, int64_t rSumOffset) {
            if (rMatched < 4 || rValid < 4) return;
            const double mean = rSumSim / rValid;
            if (mean < minSimilarity || rMatched * 5 < rValid * 4) return;
            const double qual = static_cast<double>(rMatched) * (mean - 70.0);
            if (qual > bestForShift.quality) {
                bestForShift.startIdx = rStart;
                bestForShift.endIdx = rEnd;
                bestForShift.matched = rMatched;
                bestForShift.validCount = rValid;
                bestForShift.meanSim = mean;
                bestForShift.quality = qual;
                bestForShift.sumOffset100ns = rSumOffset;
            }
        };

        for (int i = iBegin; i < iEnd; ++i) {
            const auto& sa = leftSamples[i];
            const auto& sb = rightSamples[i + d];
            if (!sa.valid || !sb.valid || sa.lowVariance || sb.lowVariance) {
                if (runMatched >= 4) commitRun(runStart, static_cast<size_t>(i - 1), runMatched, runValid, runSumSim, runSumOffset);
                runStart = static_cast<size_t>(i + 1);
                runMatched = runValid = 0;
                runSumSim = 0.0;
                runSumOffset = 0;
                continue;
            }
            const double sim = pairSim(sa, sb);
            if (sim >= minSimilarity - 3.0) {
                if (runValid == 0) runStart = static_cast<size_t>(i);
                ++runValid;
                if (sim >= minSimilarity) ++runMatched;
                runSumSim += sim;
                runSumOffset += (sb.timestamp100ns - sa.timestamp100ns);
            } else {
                if (runMatched >= 4) commitRun(runStart, static_cast<size_t>(i - 1), runMatched, runValid, runSumSim, runSumOffset);
                runStart = static_cast<size_t>(i + 1);
                runMatched = runValid = 0;
                runSumSim = 0.0;
                runSumOffset = 0;
            }
        }
        if (runMatched >= 4) {
            commitRun(runStart, static_cast<size_t>(iEnd - 1), runMatched, runValid, runSumSim, runSumOffset);
        }
        if (bestForShift.matched >= 4) {
            candidates.push_back(bestForShift);
        }
    }

    if (candidates.empty()) {
        result.status = PartialClipStatus::NotFound;
        return result;
    }

    std::sort(candidates.begin(), candidates.end(), [](const ShiftCandidate& a, const ShiftCandidate& b) {
        if (std::abs(a.quality - b.quality) > 1e-6) return a.quality > b.quality;
        return std::abs(a.shift) < std::abs(b.shift);
    });

    const auto& winner = candidates.front();
    // Check if a distinct shift (>= 1.5s away) has nearly identical quality (repetitive or static content).
    for (size_t k = 1; k < candidates.size(); ++k) {
        if (std::abs(candidates[k].shift - winner.shift) >= 3) {
            if (candidates[k].quality >= winner.quality * 0.90 && candidates[k].meanSim >= winner.meanSim - 1.5) {
                result.status = PartialClipStatus::Ambiguous;
                result.similarity = winner.meanSim;
                return result;
            }
        }
    }

    // Check temporal variation across the matched segment so two static title cards are not treated as an aligned clip.
    double minSelfSim = 100.0;
    for (size_t i = winner.startIdx; i < winner.endIdx; ++i) {
        if (leftSamples[i].valid && !leftSamples[i].lowVariance &&
            leftSamples[winner.endIdx].valid && !leftSamples[winner.endIdx].lowVariance) {
            minSelfSim = std::min(minSelfSim, PerceptualHash::CompareSignatures(leftSamples[i], leftSamples[winner.endIdx]));
        }
    }
    if (minSelfSim > 99.5) {
        result.status = PartialClipStatus::Ambiguous;
        result.similarity = winner.meanSim;
        return result;
    }

    const int64_t rawOffset = winner.sumOffset100ns / static_cast<int64_t>(winner.validCount);
    result.offset100ns = ((rawOffset + (rawOffset >= 0 ? 50000LL : -50000LL)) / 100000LL) * 100000LL;
    result.overlapStartLeft100ns = leftSamples[winner.startIdx].timestamp100ns;
    result.overlapDuration100ns = std::max<int64_t>(
        step100ns * winner.validCount,
        leftSamples[winner.endIdx].timestamp100ns - leftSamples[winner.startIdx].timestamp100ns + step100ns);
    result.similarity = winner.meanSim;
    result.matchedSamples = winner.matched;
    result.overlapSamples = winner.validCount;

    const bool sameDuration = std::abs(leftInfo.duration100ns - rightInfo.duration100ns) <= 2500000LL;
    const bool zeroOffset = std::abs(result.offset100ns) <= 500000LL;
    const bool fullCoverage = winner.validCount + 2 >= static_cast<unsigned>(std::min(nL, nR));
    result.status = (sameDuration && zeroOffset && fullCoverage)
        ? PartialClipStatus::FullTimelineMatch
        : PartialClipStatus::PartialOverlap;
    return result;
}
