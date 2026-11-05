#pragma once

#include "platform/Compat.h"

#include <cstdint>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include <array>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <chrono>
#include <memory>
#include <functional>
#include <set>
#include <map>
#include <iterator>
#include <cwchar>
#include <cstring>

// Thumbnail constants (16:9 crisp preview for bottom list)
constexpr int THUMB_W = 96;
constexpr int THUMB_H = 54;

struct FileIdentity {
    uint64_t volume = 0;
    uint64_t index = 0;
    uint64_t size = 0;
    uint64_t modified = 0;
    bool valid = false;
    bool operator==(const FileIdentity&) const = default;
};

inline std::string ReviewPairKey(const FileIdentity& a,const FileIdentity& b) {
    if(!a.valid || !b.valid) return {};
    auto key=[](const FileIdentity& value) { return std::to_string(value.volume)+":"+std::to_string(value.index)+":"+std::to_string(value.size)+":"+std::to_string(value.modified); };
    auto left=key(a),right=key(b); if(right<left) std::swap(left,right); return left+"/"+right;
}
enum class ReviewDecision { Unreviewed=0, Reviewed=1, KeepBoth=2, Later=3 };
using ReviewDecisions=std::map<std::string,ReviewDecision>;

struct FrameSignature {
    uint64_t dHashH = 0;          // 9x8 horizontal gradient hash (64 bits)
    uint64_t dHashV = 0;          // 8x9 vertical gradient hash (64 bits)
    uint64_t aHash  = 0;          // 8x8 luminance average hash (64 bits)
    uint8_t  rgbGrid[16 * 3]{};   // 4x4 spatial RGB averages (48 bytes)

    // Stage-2 foreground / subject ROI & local-detail verification (IMG5)
    uint64_t subjDHashH = 0;      // Foreground-normalized subject ROI 9x8 horizontal dHash
    uint64_t subjDHashV = 0;      // Foreground-normalized subject ROI 8x9 vertical dHash
    uint64_t subjAHash  = 0;      // Foreground-normalized subject ROI 8x8 average hash
    uint64_t edgeHashH  = 0;      // Subject ROI 8x8 horizontal gradient energy hash
    uint64_t edgeHashV  = 0;      // Subject ROI 8x8 vertical gradient energy hash
    uint64_t edgeAHash  = 0;      // Subject ROI 8x8 total edge/texture distribution hash
    uint64_t fgCellMask64 = 0;    // 64-bit foreground activity mask on 8x8 subject grid
    uint8_t  subjRgbGrid[16 * 3]{}; // Subject ROI 4x4 spatial RGB averages (48 bytes)
    uint16_t subjFgMask = 0;      // 16-bit foreground mask for 4x4 subjRgbGrid
    uint16_t bgCoherencePermille = 0; // Backdrop coherence indicator (0..1000)
    bool     hasSubjectDetail = false;
    uint8_t spatialLuma[144]{};
    uint8_t centerSpatialLuma[144]{};
    bool hasSpatialDetail = false;

    uint64_t centerDHashH = 0;    // 3.75% inset center-crop horizontal dHash
    uint64_t centerDHashV = 0;    // 3.75% inset center-crop vertical dHash
    uint64_t centerAHash  = 0;    // 3.75% inset center-crop average hash
    uint8_t  centerRgbGrid[16 * 3]{};
    uint64_t centerSubjDHashH = 0;
    uint64_t centerSubjDHashV = 0;
    uint64_t centerSubjAHash  = 0;
    uint64_t centerEdgeHashH  = 0;
    uint64_t centerEdgeHashV  = 0;
    uint64_t centerEdgeAHash  = 0;
    uint64_t centerFgCellMask64 = 0;
    uint8_t  centerSubjRgbGrid[16 * 3]{};
    uint16_t centerSubjFgMask = 0;
    uint16_t centerBgCoherencePermille = 0;
    bool     hasCenterCrop = false;
    bool     valid  = false;
    bool     lowVariance = false; // True if frame is nearly solid black/white
    int64_t  timestamp100ns = -1;
    std::vector<FrameSignature> normalized; // Eight consistently ordered border-normalized orientations.
};

enum class ImageEvidenceKind : uint8_t {
    StaticImage = 0,
    AnimatedImage = 1,
    MultiPageDocument = 2,
    MultiSizeIcon = 3,
    CompositeDocument = 4,
    TonemappedHdr = 5,
    RawEmbeddedPreview = 6,
    RawDeveloped = 7,
    VectorRasterized = 8
};

struct VideoFileInfo {
    std::wstring fullPath;
    std::wstring fileName;
    std::wstring folderPath;
    uint64_t     fileSize      = 0;      // Exact size in bytes
    int64_t      duration100ns = 0;      // Duration in 100-nanosecond units (10,000,000 = 1 sec)
    uint32_t     width         = 0;
    uint32_t     height        = 0;
    uint64_t     lastWriteTime = 0;
    FileIdentity identity;
    bool         protectedFile = false;
    bool         protectedByFolder = false;
    HRESULT      error = S_OK;
    double       fps = 0;
    std::wstring codec;
    uint32_t videoBitrate = 0, audioSampleRate = 0;
    int audioChannels = -1;
    std::wstring audioCodec;
    std::string  contentHash;
    std::vector<FrameSignature> samples;
    int comparisonRoot = -1;
    ImageEvidenceKind imageKind = ImageEvidenceKind::StaticImage;
    uint32_t imageFrameCount = 1;

    bool         metadataLoaded = false;
    bool         frameLoaded    = false;
    bool         corrupted      = false;
    bool         deleted        = false;

    FrameSignature sigFirst;             // Signature at t = 0.0s (very first frame)
    FrameSignature sigAlt;               // Signature at t = ~0.5s (fallback if t=0 is pure black fade-in)

    // 96x54 BGRA32 top-down thumbnail pixels for bottom list
    std::vector<uint8_t> thumbBGRA;

    double DurationSeconds() const {
        return static_cast<double>(duration100ns) / 10000000.0;
    }

    uint64_t PixelCount() const {
        return static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
    }
};

enum class SortOrder {
    SizeDesc = 0,       // Boyut (Büyükten Küçüğe) - Default
    SizeAsc,            // Boyut (Küçükten Büyüğe)
    DurationDesc,       // Süre (Uzundan Kısaya)
    DurationAsc,        // Süre (Kısadan Uzuna)
    ResolutionDesc,     // Çözünürlük (Yüksekten Düşüğe)
    ResolutionAsc,      // Çözünürlük (Düşükten Yükseğe)
    NameAsc             // Dosya Adı (A -> Z)
};

struct ScanCriteria {
    bool images=false;
    std::set<std::string> ignoredPairs;
    bool   matchSize       = true;
    bool   matchDuration   = false;
    bool   matchResolution = false;
    bool   matchFirstFrame = false;
    bool   matchContent = false;
    bool   matchVisualSamples = false;
    bool   matchTransforms = true;
    bool   matchFileName   = false;
    bool   matchExtension  = false;
    bool   crossFolderOnly = false;
    std::vector<std::wstring> comparisonRoots;
    bool   loadPreviews = true;
    unsigned workerCount = 4;
    std::vector<std::wstring> excludedFolders;
    std::vector<std::wstring> protectedFolders;
    std::vector<std::wstring> extensions;
    std::vector<std::wstring> excludedExtensions;
    std::vector<std::wstring> nonRecursiveFolders;
    uint64_t minFileSize = 1;
    uint64_t maxFileSize = UINT64_MAX;

    // Tolerances
    int64_t  durationTol100ns     = 2000000;  // Default ±0.20s (2,000,000 * 100ns)
    double   minFrameSimilarity   = 90.0;     // Default 90.0% similarity

    bool   includeSubfolders      = true;
    SortOrder sortOrder           = SortOrder::SizeDesc;
    std::map<uint64_t, int> volumeTierOverrides;
};

struct VideoGroup {
    std::vector<int> fileIndices;   // Indices into allFiles, sorted by active SortOrder
    int leftSlot  = 0;              // Default 0 (first item in sorted order)
    int rightSlot = 1;              // Default 1 (second item in sorted order)
    double bestFrameSim = 100.0;    // Similarity score for display
    bool manualReference = false;   // Slot 0 was chosen by the user (Make reference, Open two files); rules leave it in place
};

enum class PartialClipStatus {
    NotFound = 0,
    Ambiguous = 1,
    PartialOverlap = 2,
    FullTimelineMatch = 3
};

struct PartialClipAlignment {
    PartialClipStatus status = PartialClipStatus::NotFound;
    int64_t offset100ns = 0;           // rightTime = leftTime + offset100ns
    int64_t overlapStartLeft100ns = 0;
    int64_t overlapDuration100ns = 0;
    double similarity = 0.0;
    unsigned matchedSamples = 0;
    unsigned overlapSamples = 0;
};

// Formatting helpers
inline std::wstring FormatBytes(uint64_t bytes) {
    wchar_t buf[128];
    double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
        double gb = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
        std::swprintf(buf, std::size(buf), L"%.2f GB (%llu B)", gb, static_cast<unsigned long long>(bytes));
    } else if (bytes >= 1024ULL * 1024ULL) {
        std::swprintf(buf, std::size(buf), L"%.2f MB (%llu B)", mb, static_cast<unsigned long long>(bytes));
    } else if (bytes >= 1024ULL) {
        double kb = static_cast<double>(bytes) / 1024.0;
        std::swprintf(buf, std::size(buf), L"%.1f KB (%llu B)", kb, static_cast<unsigned long long>(bytes));
    } else {
        std::swprintf(buf, std::size(buf), L"%llu B", static_cast<unsigned long long>(bytes));
    }
    return buf;
}

inline std::wstring FormatBytesShort(uint64_t bytes) {
    wchar_t buf[64];
    if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
        double gb = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
        std::swprintf(buf, std::size(buf), L"%.2f GB", gb);
    } else if (bytes >= 1024ULL * 1024ULL) {
        double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
        std::swprintf(buf, std::size(buf), L"%.2f MB", mb);
    } else {
        double kb = static_cast<double>(bytes) / 1024.0;
        std::swprintf(buf, std::size(buf), L"%.1f KB", kb);
    }
    return buf;
}

inline std::wstring FormatDuration(int64_t duration100ns, bool includeTenths = true) {
    if (duration100ns < 0) duration100ns = 0;
    int64_t totalMs = duration100ns / 10000;
    int hours   = static_cast<int>(totalMs / 3600000);
    int minutes = static_cast<int>((totalMs % 3600000) / 60000);
    int seconds = static_cast<int>((totalMs % 60000) / 1000);
    int tenths  = static_cast<int>((totalMs % 1000) / 100);

    wchar_t buf[64];
    if (includeTenths) {
        if (hours > 0) {
            std::swprintf(buf, std::size(buf), L"%02d:%02d:%02d.%d", hours, minutes, seconds, tenths);
        } else {
            std::swprintf(buf, std::size(buf), L"%02d:%02d.%d", minutes, seconds, tenths);
        }
    } else {
        if (hours > 0) {
            std::swprintf(buf, std::size(buf), L"%02d:%02d:%02d", hours, minutes, seconds);
        } else {
            std::swprintf(buf, std::size(buf), L"%02d:%02d", minutes, seconds);
        }
    }
    return buf;
}
