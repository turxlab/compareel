#pragma once
#include "Common.h"

enum class StorageTier {
    Unknown = 0,
    Rotational = 1,
    NetworkOrRemovable = 2,
    SolidState = 3
};

namespace Platform {

enum class ComApartment {
    ApartmentThreaded,
    MultiThreaded
};

class ComInit {
public:
    explicit ComInit(ComApartment apartment = ComApartment::MultiThreaded);
    ~ComInit();
    ComInit(const ComInit&) = delete;
    ComInit& operator=(const ComInit&) = delete;

    HRESULT hr() const { return hr_; }
    bool succeeded() const { return SUCCEEDED(hr_); }

private:
    HRESULT hr_ = E_FAIL;
};

class MediaRuntime {
public:
    MediaRuntime();
    ~MediaRuntime();
    MediaRuntime(const MediaRuntime&) = delete;
    MediaRuntime& operator=(const MediaRuntime&) = delete;

    HRESULT hr() const { return hr_; }
    bool succeeded() const { return SUCCEEDED(hr_); }

private:
    HRESULT hr_ = E_FAIL;
};

uint64_t AvailablePhysicalMemoryBytes(uint64_t fallback = 0);
StorageTier DetectStorageTier(const std::wstring& path);

std::wstring NativeFilePath(const std::wstring& path);
// The path as the UTF-8 bytes FFmpeg opens: UTF-8 that FFmpeg converts back on Windows, the file's exact name bytes elsewhere.
std::string FileNameBytes(const std::wstring& path);
std::wstring ToLowerInvariant(std::wstring s);
std::wstring NormalizePathKey(const std::wstring& path);
// How the platform's usual file systems match names: Windows and macOS ignore case (macOS also treats composed and
// decomposed accents alike); Linux compares names exactly.
enum class NameMatching { IgnoreCase, IgnoreCaseAndAccentForm, Exact };
NameMatching FileNameMatching();
// Whether a single file name (no folder) can be created: Windows forbids <>:"/\\|?*, trailing dots and spaces and
// device names such as CON; POSIX only forbids "/" and NUL.
bool IsValidFileName(const std::wstring& name);
// The names the platform's own interface uses, as they appear in English interface text:
// "Recycle Bin" and "Explorer" on Windows, "Trash" and "Finder" on macOS, "Trash" and "file manager" on Linux.
// A path handed over from another app may be a sandbox alias (a Flatpak's document portal gives
// /run/user/<uid>/doc/<id>/<name>); this returns the real path it stands for, or the path itself.
std::wstring HostPath(const std::wstring& path);
const wchar_t* TrashName();
const wchar_t* FileManagerName();
bool IsPathWithinFolder(const std::wstring& path, const std::wstring& folder);
int CompareFileNamesNatural(const std::wstring& a, const std::wstring& b);

struct DirectoryEntry {
    std::wstring fullPath;
    std::wstring fileName;
    uint64_t fileSize = 0;
    uint64_t lastWriteTime = 0;
    uint64_t createdTime = 0;
    bool isDirectory = false;
    bool isReparsePoint = false;
};

bool StatRegularFile(const std::wstring& path, uint64_t& outSize, uint64_t& outLastWriteTime, uint64_t* outCreatedTime = nullptr);
uint64_t ReadFileCreationTime(const std::wstring& path);
bool EnumerateDirectory(const std::wstring& directory,
                        std::atomic<bool>& cancelFlag,
                        const std::function<void(DirectoryEntry&&)>& onEntry,
                        HRESULT* outError = nullptr);

class ReadLock {
public:
    ReadLock() = default;
    explicit ReadLock(const std::wstring& path) { open(path); }
    ~ReadLock() { close(); }
    ReadLock(const ReadLock&) = delete;
    ReadLock& operator=(const ReadLock&) = delete;
    ReadLock(ReadLock&& other) noexcept;
    ReadLock& operator=(ReadLock&& other) noexcept;

    bool open(const std::wstring& path);
    void close();
    bool valid() const { return handle_ != nullptr; }
    HRESULT error() const { return error_; }

private:
    void* handle_ = nullptr;
    HRESULT error_ = S_OK;
};

bool RevealInFileManager(const std::wstring& path);

std::wstring SendToFolderPath();
bool IsOwnedShortcut(const std::wstring& path, const wchar_t* marker);
bool InstallOwnedShortcut(const std::wstring& path,
                          const std::wstring& nativePath,
                          const std::wstring& nativeExecutable,
                          const wchar_t* arguments,
                          const wchar_t* marker);

std::wstring FormatErrorMessage(HRESULT error);
FileIdentity ReadFileIdentity(const std::wstring& path, DWORD* error = nullptr);
std::string HashFileSha256(const std::wstring& path, std::atomic<bool>& cancel, HRESULT& error, const FileIdentity& expected = {});
HRESULT RemoveFile(HWND owner, const std::wstring& path, bool recycle, const FileIdentity& expected = {});

} // namespace Platform

inline bool SamePhysicalFile(const VideoFileInfo& a, const VideoFileInfo& b) {
    if (!a.identity.valid || !b.identity.valid) return false;
    if (HasUniqueFileIndex(a.identity) && HasUniqueFileIndex(b.identity)) {
        return a.identity.volume == b.identity.volume && a.identity.index == b.identity.index;
    }
    if (HasUniqueFileIndex(a.identity) != HasUniqueFileIndex(b.identity)) {
        return false;
    }
    return !a.fullPath.empty() && !b.fullPath.empty() &&
           Platform::NormalizePathKey(a.fullPath) == Platform::NormalizePathKey(b.fullPath);
}

inline std::wstring PhysicalFileKey(const VideoFileInfo& file) {
    if (HasUniqueFileIndex(file.identity)) {
        return std::to_wstring(file.identity.volume) + L":" + std::to_wstring(file.identity.index);
    }
    return L"path:" + Platform::NormalizePathKey(file.fullPath);
}

inline std::string FileReviewId(const VideoFileInfo& file) {
    if (!file.identity.valid) return {};
    if (HasUniqueFileIndex(file.identity)) return FileReviewId(file.identity);
    return FileReviewId(file.identity, Platform::NormalizePathKey(file.fullPath));
}

inline std::string ReviewPairKey(const VideoFileInfo& a, const VideoFileInfo& b) {
    if (!a.identity.valid || !b.identity.valid) return {};
    auto left = FileReviewId(a), right = FileReviewId(b);
    if (left.empty() || right.empty()) return {};
    if (right < left) std::swap(left, right);
    return left + "/" + right;
}
