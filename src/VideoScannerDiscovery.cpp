#include "VideoScannerInternal.h"
#include "platform/Platform.h"
#include "FileOperations.h"
#include <unordered_set>
#include <map>

namespace VideoScannerInternal {

std::wstring ToLowerW(std::wstring s) {
    return Platform::ToLowerInvariant(std::move(s));
}

std::wstring NormalizePathKey(const std::wstring& path) {
    return Platform::NormalizePathKey(path);
}

bool WithinFolder(const std::wstring& path, const std::wstring& folder) {
    return Platform::IsPathWithinFolder(path, folder);
}

std::wstring NormalizeExtW(std::wstring ext) {
    ext = ToLowerW(std::move(ext));
    while (!ext.empty() && (ext.front() == L' ' || ext.front() == L'\t' || ext.front() == L'*')) {
        ext.erase(ext.begin());
    }
    while (!ext.empty() && (ext.back() == L' ' || ext.back() == L'\t')) {
        ext.pop_back();
    }
    if (!ext.empty() && ext.front() != L'.') {
        ext.insert(ext.begin(), L'.');
    }
    return ext;
}

std::wstring FileStemLowerW(const std::wstring& fileName) {
    const auto dot = fileName.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0) return ToLowerW(fileName);
    return ToLowerW(fileName.substr(0, dot));
}

std::wstring FileExtLowerW(const std::wstring& fileName) {
    const auto dot = fileName.find_last_of(L'.');
    if (dot == std::wstring::npos) return {};
    return ToLowerW(fileName.substr(dot));
}

bool MatchesExtensionFilter(const std::wstring& fname, const ScanCriteria* criteria) {
    if (!criteria) return true;
    const auto ext = FileExtLowerW(fname);
    if (!criteria->excludedExtensions.empty()) {
        for (const auto& rawExcl : criteria->excludedExtensions) {
            if (NormalizeExtW(rawExcl) == ext) return false;
        }
    }
    if (!criteria->extensions.empty()) {
        bool matched = false;
        for (const auto& rawInc : criteria->extensions) {
            if (NormalizeExtW(rawInc) == ext) {
                matched = true;
                break;
            }
        }
        if (!matched) return false;
    }
    return true;
}

} // namespace VideoScannerInternal

using namespace VideoScannerInternal;

bool VideoScanner::IsWithinFolder(const std::wstring& path, const std::wstring& folder) {
    return WithinFolder(path, folder);
}

bool VideoScanner::IsVideoExtension(const std::wstring& fileName) {
    auto dotPos = fileName.find_last_of(L'.');
    if (dotPos == std::wstring::npos) return false;
    std::wstring ext = ToLowerW(fileName.substr(dotPos));
    static const std::unordered_set<std::wstring> kVideoExts = {
        L".mp4", L".mkv", L".avi", L".mov", L".wmv", L".webm",
        L".flv", L".m4v", L".mpg", L".mpeg", L".ts", L".mts",
        L".m2ts", L".3gp", L".vob", L".ogv", L".divx", L".asf"
    };
    return kVideoExts.find(ext) != kVideoExts.end();
}

bool VideoScanner::IsImageExtension(const std::wstring& fileName) {
    const auto dot = fileName.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    const auto ext = ToLowerW(fileName.substr(dot));
    static const std::unordered_set<std::wstring> kImageExts = {
        // Common static & web formats + aliases
        L".jpg", L".jpeg", L".jpe", L".jfif",
        L".png", L".apng",
        L".webp",
        L".heic", L".heif", L".hif",
        L".avif", L".avifs",
        L".bmp", L".dib",
        L".gif",
        L".tif", L".tiff",
        L".tga", L".icb", L".vda", L".vst",
        L".ico", L".cur", L".icns",
        // Netpbm & X11/wireless bitmaps
        L".pbm", L".pgm", L".ppm", L".pnm", L".pam", L".pfm", L".phm",
        L".xbm", L".xpm", L".wbmp",
        // Vector
        L".svg", L".svgz",
        // Next-gen & archival JPEG families
        L".jxl",
        L".jp2", L".j2k", L".jpc", L".jpf", L".jpx", L".j2c",
        L".jls",
#ifdef _WIN32
        L".jxr", L".wdp", L".hdp", // JPEG XR: only Windows has a decoder (WIC); macOS and Linux have none
#endif
        // Professional / HDR / textures / legacy raster
        L".psd", L".psb",
        L".exr", L".hdr", L".pic",
        L".dds", L".pcx", L".qoi",
        L".sgi", L".rgb", L".bw", L".rgba",
        L".ras", L".sun", L".dpx", L".iff", L".lbm",
        // Camera RAW formats
        L".dng", L".cr2", L".cr3", L".nef", L".nrw",
        L".arw", L".srf", L".sr2", L".raf", L".orf",
        L".rw2", L".pef", L".srw", L".erf", L".kdc",
        L".dcr", L".3fr", L".mef", L".mos", L".iiq", L".raw"
    };
    return kImageExts.find(ext) != kImageExts.end();
}

std::vector<VideoFileInfo> VideoScanner::DiscoverVideos(
    const std::vector<std::wstring>& folders,
    bool includeSubfolders,
    std::atomic<bool>& cancelFlag,
    ProgressCallback onProgress,
    const ScanCriteria* criteria,
    std::vector<ScanIssue>* issues)
{
    std::vector<VideoFileInfo> result;
    std::unordered_set<std::wstring> visitedFiles;
    std::unordered_set<std::wstring> visitedDirs;
    std::map<std::pair<uint64_t, uint64_t>, size_t> physicalFiles;

    std::unordered_set<std::wstring> nonRecursiveSet;
    if (criteria) {
        for (const auto& nr : criteria->nonRecursiveFolders) {
            if (!nr.empty()) nonRecursiveSet.insert(NormalizePathKey(nr));
        }
    }

    auto tryAddDiscoveredFile = [&](std::wstring fullPath, std::wstring fname, const std::wstring& parentDir,
                                    uint64_t fsize, uint64_t lastWriteTime) {
        if (criteria && std::any_of(criteria->excludedFolders.begin(), criteria->excludedFolders.end(),
            [&](const auto& folder) { return WithinFolder(fullPath, folder); })) return;
        if (!(criteria && criteria->images ? IsImageExtension(fname) : IsVideoExtension(fname))) return;
        if (fsize == 0) return;
        if (criteria && (fsize < criteria->minFileSize || fsize > criteria->maxFileSize)) return;
        if (!MatchesExtensionFilter(fname, criteria)) return;

        DWORD identityError = 0;
        auto identity = FileOperations::ReadIdentity(fullPath, &identityError);
        if (!identity.valid) {
            if (issues) issues->push_back({fullPath, HRESULT_FROM_WIN32(identityError)});
            return;
        }
        bool protectedPath = criteria && std::any_of(criteria->protectedFolders.begin(), criteria->protectedFolders.end(),
            [&](const auto& folder) { return WithinFolder(fullPath, folder); });
        auto [physical, inserted] = physicalFiles.emplace(std::make_pair(identity.volume, identity.index), result.size());
        if (!inserted) {
            if (protectedPath) result[physical->second].protectedFile = result[physical->second].protectedByFolder = true;
            return;
        }
        std::wstring fileKey = NormalizePathKey(fullPath);
        if (visitedFiles.insert(fileKey).second) {
            VideoFileInfo v{};
            v.fullPath = std::move(fullPath);
            v.fileName = std::move(fname);
            v.folderPath = parentDir;
            v.fileSize = fsize;
            v.identity = identity;
            v.protectedFile = protectedPath;
            v.protectedByFolder = v.protectedFile;
            v.lastWriteTime = lastWriteTime;
            result.push_back(std::move(v));

            if (onProgress && (result.size() % 100 == 0)) {
                ScanProgress sp{};
                sp.stage = 1;
                sp.current = static_cast<int>(result.size());
                sp.total = static_cast<int>(result.size());
                sp.statusText = L"Videolar bulunuyor... (" + std::to_wstring(result.size()) + L" dosya)";
                onProgress(sp);
            }
        }
    };

    std::vector<std::pair<std::wstring, bool>> dirQueue;
    for (const auto& f : folders) {
        if (f.empty()) continue;
        std::wstring clean = f;
        while (clean.size() > 3 && (clean.back() == L'\\' || clean.back() == L'/')) {
            clean.pop_back();
        }
        uint64_t fsize = 0, lastWrite = 0;
        if (Platform::StatRegularFile(clean, fsize, lastWrite)) {
            auto slash = clean.find_last_of(L"\\/");
            std::wstring fname = (slash == std::wstring::npos) ? clean : clean.substr(slash + 1);
            std::wstring parentDir = (slash == std::wstring::npos) ? L"." : clean.substr(0, slash);
            tryAddDiscoveredFile(clean, std::move(fname), parentDir, fsize, lastWrite);
            continue;
        }
        std::wstring key = NormalizePathKey(clean);
        if (visitedDirs.insert(key).second) {
            bool recurseThisRoot = includeSubfolders && (nonRecursiveSet.find(key) == nonRecursiveSet.end());
            dirQueue.emplace_back(clean, recurseThisRoot);
        }
    }

    size_t qIdx = 0;
    while (qIdx < dirQueue.size() && !cancelFlag.load(std::memory_order_relaxed)) {
        std::wstring currentDir = dirQueue[qIdx].first;
        bool recurseCurrent = dirQueue[qIdx].second;
        ++qIdx;
        if (criteria && std::any_of(criteria->excludedFolders.begin(), criteria->excludedFolders.end(),
            [&](const auto& folder) { return WithinFolder(currentDir, folder); })) continue;

        HRESULT enumError = S_OK;
        if (!Platform::EnumerateDirectory(currentDir, cancelFlag, [&](Platform::DirectoryEntry&& entry) {
            if (entry.isDirectory) {
                if (recurseCurrent && !entry.isReparsePoint) {
                    std::wstring dirKey = NormalizePathKey(entry.fullPath);
                    if (visitedDirs.insert(dirKey).second) {
                        bool subRecurse = recurseCurrent && (nonRecursiveSet.find(dirKey) == nonRecursiveSet.end());
                        dirQueue.emplace_back(std::move(entry.fullPath), subRecurse);
                    }
                }
            } else {
                tryAddDiscoveredFile(std::move(entry.fullPath), std::move(entry.fileName), currentDir, entry.fileSize, entry.lastWriteTime);
            }
        }, &enumError) && issues && FAILED(enumError)) {
            issues->push_back({currentDir, enumError});
        }
    }

    return result;
}
