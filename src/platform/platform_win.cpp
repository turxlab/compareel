#include "platform/WinApi.h"
#include "platform/Platform.h"
#include <cwctype>
#include <filesystem>
#include <winioctl.h>
#include <wrl/client.h>

namespace Platform {

ComInit::ComInit(ComApartment apartment)
    : hr_(CoInitializeEx(nullptr, apartment == ComApartment::ApartmentThreaded ? COINIT_APARTMENTTHREADED : COINIT_MULTITHREADED)) {}

ComInit::~ComInit() {
    if (SUCCEEDED(hr_)) CoUninitialize();
}

MediaRuntime::MediaRuntime()
    : hr_(MFStartup(MF_VERSION)) {}

MediaRuntime::~MediaRuntime() {
    if (SUCCEEDED(hr_)) MFShutdown();
}

uint64_t AvailablePhysicalMemoryBytes(uint64_t fallback) {
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    return GlobalMemoryStatusEx(&memory) ? memory.ullAvailPhys : fallback;
}

StorageTier DetectStorageTier(const std::wstring& path) {
    if (path.empty()) return StorageTier::Unknown;
    if ((path.rfind(L"\\\\", 0) == 0 && path.rfind(L"\\\\?\\", 0) != 0 && path.rfind(L"\\\\.\\", 0) != 0) ||
        path.rfind(L"\\\\?\\UNC\\", 0) == 0) {
        return StorageTier::NetworkOrRemovable;
    }
    wchar_t volumeRoot[MAX_PATH]{};
    if (!GetVolumePathNameW(path.c_str(), volumeRoot, MAX_PATH) || !volumeRoot[0]) {
        return StorageTier::Unknown;
    }
    const UINT driveType = GetDriveTypeW(volumeRoot);
    if (driveType == DRIVE_REMOTE || driveType == DRIVE_REMOVABLE || driveType == DRIVE_CDROM) {
        return StorageTier::NetworkOrRemovable;
    }
    if (driveType != DRIVE_FIXED) {
        return StorageTier::Unknown;
    }
    std::wstring devicePath;
    wchar_t volumeGuid[MAX_PATH]{};
    if (GetVolumeNameForVolumeMountPointW(volumeRoot, volumeGuid, MAX_PATH) && volumeGuid[0]) {
        devicePath = volumeGuid;
        while (!devicePath.empty() && (devicePath.back() == L'\\' || devicePath.back() == L'/')) {
            devicePath.pop_back();
        }
    } else {
        std::wstring root(volumeRoot);
        while (!root.empty() && (root.back() == L'\\' || root.back() == L'/')) {
            root.pop_back();
        }
        if (root.rfind(L"\\\\?\\", 0) == 0) devicePath = L"\\\\.\\" + root.substr(4);
        else devicePath = L"\\\\.\\" + root;
    }
    HANDLE hDevice = CreateFileW(devicePath.c_str(), 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, 0, nullptr);
    if (hDevice == INVALID_HANDLE_VALUE) {
        return StorageTier::Unknown;
    }
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceSeekPenaltyProperty;
    query.QueryType = PropertyStandardQuery;
    DEVICE_SEEK_PENALTY_DESCRIPTOR seekDesc{};
    DWORD bytesReturned = 0;
    if (DeviceIoControl(hDevice, IOCTL_STORAGE_QUERY_PROPERTY,
            &query, sizeof(query), &seekDesc, sizeof(seekDesc), &bytesReturned, nullptr) &&
        bytesReturned >= sizeof(DEVICE_SEEK_PENALTY_DESCRIPTOR)) {
        CloseHandle(hDevice);
        return seekDesc.IncursSeekPenalty ? StorageTier::Rotational : StorageTier::SolidState;
    }
    query.PropertyId = StorageDeviceTrimProperty;
    DEVICE_TRIM_DESCRIPTOR trimDesc{};
    bytesReturned = 0;
    if (DeviceIoControl(hDevice, IOCTL_STORAGE_QUERY_PROPERTY,
            &query, sizeof(query), &trimDesc, sizeof(trimDesc), &bytesReturned, nullptr) &&
        bytesReturned >= sizeof(DEVICE_TRIM_DESCRIPTOR)) {
        CloseHandle(hDevice);
        return trimDesc.TrimEnabled ? StorageTier::SolidState : StorageTier::Unknown;
    }
    CloseHandle(hDevice);
    return StorageTier::Unknown;
}

std::string FileNameBytes(const std::wstring& path) {
    const auto bytes = std::filesystem::path(path).u8string();
    return std::string(bytes.begin(), bytes.end());
}

std::wstring NativeFilePath(const std::wstring& path) {
    if (path.size() < MAX_PATH - 12 || path.starts_with(L"\\\\?\\")) return path;
    const auto length = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (!length) return path;
    std::wstring full(length, L'\0');
    const auto written = GetFullPathNameW(path.c_str(), length, full.data(), nullptr);
    if (!written || written >= length) return path;
    full.resize(written);
    std::replace(full.begin(), full.end(), L'/', L'\\');
    return full.starts_with(L"\\\\") ? L"\\\\?\\UNC\\" + full.substr(2) : L"\\\\?\\" + full;
}

std::wstring ToLowerInvariant(std::wstring s) {
    if (std::all_of(s.begin(), s.end(), [](wchar_t ch) { return ch < 0x80; })) {
        for (auto& ch : s) {
            if (ch >= L'A' && ch <= L'Z') ch = ch - L'A' + L'a';
        }
        return s;
    }
    // Invariant mapping also folds non-ASCII capitals (Ç, Ö, Ş, Ü, Ğ), like case-insensitive NTFS names.
    const int size = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, s.c_str(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr, 0);
    if (size <= 0) return s;
    std::wstring lower(static_cast<size_t>(size), L'\0');
    if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, s.c_str(), static_cast<int>(s.size()), lower.data(), size, nullptr, nullptr, 0) != size) return s;
    return lower;
}

NameMatching FileNameMatching() { return NameMatching::IgnoreCase; }
std::wstring HostPath(const std::wstring& path) { return path; }
const wchar_t* TrashName() { return L"Recycle Bin"; }
const wchar_t* FileManagerName() { return L"Explorer"; }

bool IsValidFileName(const std::wstring& name) {
    if (name.empty() || name == L"." || name == L".." || name.back() == L' ' || name.back() == L'.') return false;
    for (wchar_t c : name) if (c < 32 || std::wstring_view(L"<>:\"/\\|?*").find(c) != std::wstring_view::npos) return false;
    std::wstring base = name.substr(0, name.find(L'.'));
    for (auto& c : base) c = static_cast<wchar_t>(std::towupper(c));
    if (base == L"CON" || base == L"PRN" || base == L"AUX" || base == L"NUL") return false;
    return !(base.size() == 4 && (base.starts_with(L"COM") || base.starts_with(L"LPT")) && base[3] >= L'0' && base[3] <= L'9');
}

std::wstring NormalizePathKey(const std::wstring& path) {
    wchar_t full[MAX_PATH * 4]{};
    DWORD len = GetFullPathNameW(path.c_str(), MAX_PATH * 4, full, nullptr);
    std::wstring res = (len > 0 && len < MAX_PATH * 4) ? std::wstring(full, len) : path;
    while (res.size() > 3 && (res.back() == L'\\' || res.back() == L'/')) {
        res.pop_back();
    }
    return ToLowerInvariant(res);
}

bool IsPathWithinFolder(const std::wstring& path, const std::wstring& folder) {
    if (folder.empty()) return false;
    auto root = NormalizePathKey(folder);
    auto candidate = NormalizePathKey(path);
    return candidate == root || (candidate.size() > root.size() &&
        CompareStringOrdinal(candidate.c_str(), static_cast<int>(root.size()), root.c_str(),
            static_cast<int>(root.size()), TRUE) == CSTR_EQUAL &&
        (root.back() == L'\\' || candidate[root.size()] == L'\\'));
}

int CompareFileNamesNatural(const std::wstring& a, const std::wstring& b) {
    // User-locale order: Turkish letters sort beside their base letters and "video2" precedes "video10".
    const int relation = CompareStringEx(LOCALE_NAME_USER_DEFAULT, LINGUISTIC_IGNORECASE | SORT_DIGITSASNUMBERS,
        a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), nullptr, nullptr, 0);
    if (relation == CSTR_LESS_THAN) return -1;
    if (relation == CSTR_GREATER_THAN) return 1;
    if (a < b) return -1;
    if (b < a) return 1;
    return 0;
}

bool StatRegularFile(const std::wstring& path, uint64_t& outSize, uint64_t& outLastWriteTime, uint64_t* outCreatedTime) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(NativeFilePath(path).c_str(), GetFileExInfoStandard, &fad) ||
        (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        return false;
    }
    outSize = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    outLastWriteTime = (static_cast<uint64_t>(fad.ftLastWriteTime.dwHighDateTime) << 32) |
                       fad.ftLastWriteTime.dwLowDateTime;
    if (outCreatedTime) {
        *outCreatedTime = (static_cast<uint64_t>(fad.ftCreationTime.dwHighDateTime) << 32) |
                          fad.ftCreationTime.dwLowDateTime;
    }
    return true;
}

uint64_t ReadFileCreationTime(const std::wstring& path) {
    uint64_t size = 0, modified = 0, created = 0;
    return StatRegularFile(path, size, modified, &created) ? created : 0;
}

bool EnumerateDirectory(const std::wstring& directory,
                        std::atomic<bool>& cancelFlag,
                        const std::function<void(DirectoryEntry&&)>& onEntry,
                        HRESULT* outError) {
    if (outError) *outError = S_OK;
    std::wstring searchPattern = directory;
    if (!searchPattern.empty() && searchPattern.back() != L'\\') searchPattern += L'\\';
    searchPattern += L"*";

    WIN32_FIND_DATAW fd{};
    HANDLE hFind = FindFirstFileExW(
        NativeFilePath(searchPattern).c_str(),
        FindExInfoBasic,
        &fd,
        FindExSearchNameMatch,
        nullptr,
        FIND_FIRST_EX_LARGE_FETCH);

    if (hFind == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND) {
            if (outError) *outError = HRESULT_FROM_WIN32(error);
            return false;
        }
        return true;
    }

    do {
        if (fd.cFileName[0] == L'.') {
            if (fd.cFileName[1] == L'\0' || (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0')) {
                continue;
            }
        }
        DirectoryEntry entry;
        entry.fullPath = directory;
        if (!entry.fullPath.empty() && entry.fullPath.back() != L'\\') entry.fullPath += L'\\';
        entry.fullPath += fd.cFileName;
        entry.fileName = fd.cFileName;
        entry.isDirectory = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        entry.isReparsePoint = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        if (!entry.isDirectory) {
            entry.fileSize = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            entry.lastWriteTime = (static_cast<uint64_t>(fd.ftLastWriteTime.dwHighDateTime) << 32) |
                                  fd.ftLastWriteTime.dwLowDateTime;
            entry.createdTime = (static_cast<uint64_t>(fd.ftCreationTime.dwHighDateTime) << 32) |
                                fd.ftCreationTime.dwLowDateTime;
        }
        onEntry(std::move(entry));
    } while (FindNextFileW(hFind, &fd) && !cancelFlag.load(std::memory_order_relaxed));

    DWORD enumerationError = GetLastError();
    FindClose(hFind);
    if (!cancelFlag.load() && enumerationError != ERROR_NO_MORE_FILES && enumerationError != ERROR_SUCCESS) {
        if (outError) *outError = HRESULT_FROM_WIN32(enumerationError);
        return false;
    }
    return true;
}

ReadLock::ReadLock(ReadLock&& other) noexcept
    : handle_(other.handle_), error_(other.error_) {
    other.handle_ = nullptr;
    other.error_ = S_OK;
}

ReadLock& ReadLock::operator=(ReadLock&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        error_ = other.error_;
        other.handle_ = nullptr;
        other.error_ = S_OK;
    }
    return *this;
}

bool ReadLock::open(const std::wstring& path) {
    close();
    // Same open as ReadIdentity: long-path safe, and a link is locked as itself rather than followed.
    HANDLE raw = CreateFileW(NativeFilePath(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (raw == INVALID_HANDLE_VALUE) {
        error_ = HRESULT_FROM_WIN32(GetLastError());
        return false;
    }
    handle_ = raw;
    error_ = S_OK;
    return true;
}

void ReadLock::close() {
    if (handle_) {
        CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = nullptr;
    }
}

bool RevealInFileManager(const std::wstring& path) {
    // ILCreateFromPathW refuses paths past MAX_PATH; SHParseDisplayName accepts the extended \\?\ form.
    PIDLIST_ABSOLUTE item = ILCreateFromPathW(path.c_str());
    if (!item) SHParseDisplayName(NativeFilePath(path).c_str(), nullptr, &item, 0, nullptr);
    if (!item) return false;
    SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
    ILFree(item);
    return true;
}

std::wstring SendToFolderPath() {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_SendTo, 0, nullptr, &raw)) || !raw) return {};
    std::wstring folder(raw);
    CoTaskMemFree(raw);
    return folder;
}

bool IsOwnedShortcut(const std::wstring& path, const wchar_t* marker) {
    Microsoft::WRL::ComPtr<IShellLinkW> link;
    Microsoft::WRL::ComPtr<IPersistFile> file;
    wchar_t description[128]{};
    return SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) &&
        SUCCEEDED(link.As(&file)) && file->Load(path.c_str(), STGM_READ) == S_OK &&
        SUCCEEDED(link->GetDescription(description, 128)) && std::wstring(description) == marker;
}

bool InstallOwnedShortcut(const std::wstring& path,
                          const std::wstring& nativePath,
                          const std::wstring& nativeExecutable,
                          const wchar_t* arguments,
                          const wchar_t* marker) {
    if (path.empty() || nativeExecutable.empty()) return false;
    // Inspect the shortcut itself (not its target) before overwriting.
    const auto attributes = GetFileAttributesW(nativePath.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        if ((attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) || !IsOwnedShortcut(path, marker)) {
            return false;
        }
    } else if (GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND) {
        return false;
    }
    Microsoft::WRL::ComPtr<IShellLinkW> link;
    Microsoft::WRL::ComPtr<IPersistFile> file;
    return SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) &&
        SUCCEEDED(link->SetPath(nativeExecutable.c_str())) &&
        SUCCEEDED(link->SetArguments(arguments)) &&
        SUCCEEDED(link->SetDescription(marker)) &&
        SUCCEEDED(link->SetIconLocation(nativeExecutable.c_str(), 0)) &&
        SUCCEEDED(link.As(&file)) &&
        SUCCEEDED(file->Save(path.c_str(), TRUE));
}

} // namespace Platform
