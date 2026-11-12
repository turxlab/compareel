#pragma once
#include "Common.h"
#include "PerceptualHash.h"

struct ScanMetrics {
    int64_t readMs=0, sampleMs=0, groupMs=0;
    size_t inputCandidates=0, sampleCandidates=0, durationRejected=0;
    size_t discoveredFiles=0, outsideSizeLimits=0, skippedUniqueSize=0;
    bool compactedFiles=false;
    std::vector<std::wstring> lowVarianceSkipped;
    unsigned requestedWorkers=0, readWorkers=0, sampleWorkers=0, volumeCap=0;
    int primaryStorageTier=0;
};

struct ScanProgress {
    int  stage        = 0;   // 1=Enumerating, 2=Metadata, 3=Frames/Thumbnails, 4=Grouping
    int  current      = 0;
    int  total        = 0;
    int  groupsFound  = 0;
    bool finished     = false;
    std::wstring statusText;
};

using ProgressCallback = std::function<void(const ScanProgress&)>;
using FileProcessedCallback = std::function<void(const VideoFileInfo&)>;
struct ScanIssue { std::wstring path; HRESULT error=E_FAIL; };

class VideoScanner {
public:
    static bool IsWithinFolder(const std::wstring& path,const std::wstring& folder);
    VideoScanner() = default;
    ~VideoScanner() = default;

    // Enumerate video files in the given folders
    static std::vector<VideoFileInfo> DiscoverVideos(
        const std::vector<std::wstring>& folders,
        bool includeSubfolders,
        std::atomic<bool>& cancelFlag,
        ProgressCallback onProgress = nullptr,
        const ScanCriteria* criteria = nullptr,
        std::vector<ScanIssue>* issues = nullptr);

    // Extract duration and resolution (fast header read) for a single file
    static bool ExtractMetadataOnly(VideoFileInfo& info, std::atomic<bool>* cancel = nullptr);

    // Extract metadata + first frame signature + 96x54 thumbnail for a single file
    static bool ExtractMetadataAndFrame(VideoFileInfo& info, bool sampleVideo = false,
                                        std::atomic<bool>* cancel = nullptr);

    // Execute full multi-stage parallel scan & grouping pipeline
    static void AnalyzeAndGroup(
        std::vector<VideoFileInfo>& files,
        const ScanCriteria& criteria,
        std::vector<VideoGroup>& outGroups,
        std::atomic<bool>& cancelFlag,
        ProgressCallback onProgress = nullptr,
        FileProcessedCallback onFileProcessed = nullptr,
        ScanMetrics* metrics = nullptr,
        std::function<void(VideoFileInfo&)> readImage = nullptr);

    // Regroup already analyzed candidates without reopening media (for result sorting).
    static void GroupKnownFiles(const std::vector<VideoFileInfo>& files, std::vector<int> candidates,
        const ScanCriteria& criteria, std::vector<VideoGroup>& groups, std::atomic<bool>& cancel,
        ProgressCallback progress = nullptr);

    // Sort files inside a group and sort groups according to SortOrder
    static void SortGroups(
        const std::vector<VideoFileInfo>& files,
        std::vector<VideoGroup>& groups,
        SortOrder order, bool matchTransforms = true);

    // Compute real visual/content similarity (0..100) between two files
    static double ComputePairSimilarity(
        const VideoFileInfo& a,
        const VideoFileInfo& b,
        bool matchTransforms = true);

    // Check if two files match under the active criteria
    static bool FilesMatch(
        const VideoFileInfo& a,
        const VideoFileInfo& b,
        const ScanCriteria& criteria,
        double* outFrameSim = nullptr);

    static PartialClipAlignment EstimatePartialClipAlignment(
        const std::wstring& leftPath,
        const std::wstring& rightPath,
        std::atomic<bool>* cancel = nullptr,
        double minSimilarity = 88.0);

    static bool IsVideoExtension(const std::wstring& fileName);
    static bool IsImageExtension(const std::wstring& fileName);
};
