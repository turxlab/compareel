// POSIX (Linux, macOS) implementation of Platform.h and PlatformWic.h.
#include "platform/Platform.h"
#include "platform/PlatformWic.h"
#include "platform/PosixSupport.h"
#include <QCollator>
#include <QProcess>
#include <QString>
#include <QUrl>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOBSD.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/storage/IOStorageDeviceCharacteristics.h>
#include <mach/mach.h>
#else
#include <QDBusConnection>
#include <QDBusMessage>
#include <sys/statfs.h>
#include <sys/xattr.h>
#include <sys/sysmacros.h>
#endif

namespace Platform {
using namespace Posix;

ComInit::ComInit(ComApartment) : hr_(S_OK) {}
ComInit::~ComInit() = default;
MediaRuntime::MediaRuntime() : hr_(S_OK) {}
MediaRuntime::~MediaRuntime() = default;

uint64_t AvailablePhysicalMemoryBytes(uint64_t fallback) {
#ifdef __APPLE__
    vm_statistics64_data_t stats{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&stats), &count) != KERN_SUCCESS) return fallback;
    const auto page = static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
    return (static_cast<uint64_t>(stats.free_count) + stats.inactive_count + stats.purgeable_count) * page;
#else
    // MemAvailable counts reclaimable cache, like Windows' available physical memory.
    std::ifstream meminfo("/proc/meminfo");
    std::string key; uint64_t value = 0; std::string unit;
    while (meminfo >> key >> value >> unit) {
        if (key == "MemAvailable:") return value * 1024;
    }
    const long pages = sysconf(_SC_AVPHYS_PAGES), size = sysconf(_SC_PAGESIZE);
    return pages > 0 && size > 0 ? static_cast<uint64_t>(pages) * static_cast<uint64_t>(size) : fallback;
#endif
}

#ifdef __APPLE__
namespace {
// From the volume's media ("/dev/disk3s1") the registry leads up, through an APFS container to its physical
// store, to the device that reports whether it is solid state or rotational.
StorageTier MediumType(const char* device) {
    const std::string_view prefix("/dev/");
    if (std::string_view(device).substr(0, prefix.size()) != prefix) return StorageTier::Unknown;
    const io_service_t media = IOServiceGetMatchingService(kIOMainPortDefault, IOBSDNameMatching(kIOMainPortDefault, 0, device + prefix.size()));
    if (!media) return StorageTier::Unknown;
    const auto characteristics = IORegistryEntrySearchCFProperty(media, kIOServicePlane, CFSTR(kIOPropertyDeviceCharacteristicsKey),
        kCFAllocatorDefault, kIORegistryIterateRecursively | kIORegistryIterateParents);
    IOObjectRelease(media);
    if (!characteristics) return StorageTier::Unknown;
    auto tier = StorageTier::Unknown;
    if (CFGetTypeID(characteristics) == CFDictionaryGetTypeID()) {
        const auto type = CFDictionaryGetValue(static_cast<CFDictionaryRef>(characteristics), CFSTR(kIOPropertyMediumTypeKey));
        if (type && CFGetTypeID(type) == CFStringGetTypeID()) {
            if (CFEqual(type, CFSTR(kIOPropertyMediumTypeSolidStateKey))) tier = StorageTier::SolidState;
            else if (CFEqual(type, CFSTR(kIOPropertyMediumTypeRotationalKey))) tier = StorageTier::Rotational;
        }
    }
    CFRelease(characteristics);
    return tier;
}
}
#endif

StorageTier DetectStorageTier(const std::wstring& path) {
    if (path.empty()) return StorageTier::Unknown;
    const auto native = ToUtf8(path);
    struct statfs fs{};
    if (statfs(native.c_str(), &fs) != 0) return StorageTier::Unknown;
#ifdef __APPLE__
    if (!(fs.f_flags & MNT_LOCAL)) return StorageTier::NetworkOrRemovable;
    if (fs.f_flags & MNT_REMOVABLE) return StorageTier::NetworkOrRemovable;
    return MediumType(fs.f_mntfromname);
#else
    switch (static_cast<unsigned long>(fs.f_type)) {
    case 0x6969: case 0x517B: case 0xFF534D42: case 0xFE534D42: case 0x65735546: case 0x564C: case 0x01021997:
        return StorageTier::NetworkOrRemovable; // NFS, SMB, CIFS, SMB2, FUSE, NCP, 9p
    default: break;
    }
    struct stat st{};
    if (stat(native.c_str(), &st) != 0) return StorageTier::Unknown;
    // /sys/dev/block/MAJ:MIN is the partition; its "queue" and "removable" belong to the parent disk.
    std::error_code error;
    auto device = std::filesystem::canonical("/sys/dev/block/" + std::to_string(major(st.st_dev)) + ":" + std::to_string(minor(st.st_dev)), error);
    if (error) return StorageTier::Unknown;
    for (int depth = 0; depth < 2 && !device.empty(); ++depth, device = device.parent_path()) {
        std::ifstream removable(device / "removable");
        int value = 0;
        if (removable >> value && value == 1) return StorageTier::NetworkOrRemovable;
        std::ifstream rotational(device / "queue/rotational");
        if (rotational >> value) return value ? StorageTier::Rotational : StorageTier::SolidState;
    }
    return StorageTier::Unknown;
#endif
}

std::wstring NativeFilePath(const std::wstring& path) { return path; }
// Not std::filesystem::path: on POSIX it converts through the C locale, which mangles names under LANG=C.
std::string FileNameBytes(const std::wstring& path) { return ToUtf8(path); }

std::wstring ToLowerInvariant(std::wstring s) {
    if (std::all_of(s.begin(), s.end(), [](wchar_t ch) { return ch < 0x80; })) {
        for (auto& ch : s) if (ch >= L'A' && ch <= L'Z') ch = ch - L'A' + L'a';
        return s;
    }
    return QString::fromStdWString(s).toLower().toStdWString();
}

namespace {
// macOS volumes are case-insensitive by default and treat composed and decomposed accents ("é" as one or two
// code points) as the same name, so its keys use both forms alike; Linux file systems are case-sensitive.
std::wstring CaseKey(std::wstring path) {
#ifdef __APPLE__
    return QString::fromStdWString(path).normalized(QString::NormalizationForm_C).toLower().toStdWString();
#else
    return path;
#endif
}

// Finder's "._name" AppleDouble companions share the media extension but hold only metadata, and trash and
// indexing folders hold no user media; Explorer hides the Windows counterparts as system files.
bool IsSystemEntry(const std::string& name, bool directory) {
    if (!directory) return name.size() > 2 && name.compare(0, 2, "._") == 0;
    return name == ".Trash" || name == ".Trashes" || name.compare(0, 7, ".Trash-") == 0 || name == ".Spotlight-V100" ||
        name == ".fseventsd" || name == ".TemporaryItems" || name == ".DocumentRevisions-V100" || name == "lost+found";
}
}

bool IsValidFileName(const std::wstring& name) {
    return !name.empty() && name != L"." && name != L".." && name.find_first_of(std::wstring(L"/\0", 2)) == std::wstring::npos;
}

std::wstring HostPath(const std::wstring& path) {
#ifdef __APPLE__
    return path;
#else
    // The document portal labels each file it exposes with the real path it stands for. A Flatpak with host
    // access sees that path itself, and folders must be scanned there: the alias holds only what was dropped.
    if (path.rfind(L"/run/user/", 0) != 0 || path.find(L"/doc/") == std::wstring::npos) return path;
    char host[4096];
    const auto length = getxattr(ToUtf8(path).c_str(), "user.document-portal.host-path", host, sizeof(host) - 1);
    if (length <= 0) return path;
    std::string value(host, static_cast<size_t>(length));
    while (!value.empty() && value.back() == '\0') value.pop_back();
    struct stat st{};
    return !value.empty() && stat(value.c_str(), &st) == 0 ? FromUtf8(value) : path;
#endif
}
const wchar_t* TrashName() { return L"Trash"; }
const wchar_t* FileManagerName() {
#ifdef __APPLE__
    return L"Finder";
#else
    return L"file manager";
#endif
}

NameMatching FileNameMatching() {
#ifdef __APPLE__
    return NameMatching::IgnoreCaseAndAccentForm;
#else
    return NameMatching::Exact;
#endif
}

std::wstring NormalizePathKey(const std::wstring& path) {
    std::error_code error;
    auto absolute = std::filesystem::absolute(std::filesystem::path(ToUtf8(path)), error);
    std::wstring result = error ? path : FromUtf8(absolute.lexically_normal().string());
    while (result.size() > 1 && result.back() == L'/') result.pop_back();
    return CaseKey(result);
}

bool IsPathWithinFolder(const std::wstring& path, const std::wstring& folder) {
    if (folder.empty()) return false;
    const auto root = NormalizePathKey(folder);
    const auto candidate = NormalizePathKey(path);
    return candidate == root || (candidate.size() > root.size() && candidate.compare(0, root.size(), root) == 0 &&
        (root.back() == L'/' || candidate[root.size()] == L'/'));
}

int CompareFileNamesNatural(const std::wstring& a, const std::wstring& b) {
    // User-locale order with numbers compared by value ("video2" before "video10").
    thread_local QCollator collator = [] { QCollator c; c.setNumericMode(true); c.setCaseSensitivity(Qt::CaseInsensitive); return c; }();
    const int relation = collator.compare(QString::fromStdWString(a), QString::fromStdWString(b));
    if (relation != 0) return relation < 0 ? -1 : 1;
    if (a < b) return -1;
    if (b < a) return 1;
    return 0;
}

bool StatRegularFile(const std::wstring& path, uint64_t& outSize, uint64_t& outLastWriteTime, uint64_t* outCreatedTime) {
    struct stat st{};
    if (stat(ToUtf8(path).c_str(), &st) != 0 || S_ISDIR(st.st_mode)) return false;
    outSize = static_cast<uint64_t>(st.st_size);
#ifdef __APPLE__
    outLastWriteTime = FileTimeFromTimespec(st.st_mtimespec);
    if (outCreatedTime) *outCreatedTime = st.st_birthtimespec.tv_sec > 0 ? FileTimeFromTimespec(st.st_birthtimespec) : 0;
#else
    outLastWriteTime = FileTimeFromTimespec(st.st_mtim);
    if (outCreatedTime) *outCreatedTime = ReadFileCreationTime(path);
#endif
    return true;
}

uint64_t ReadFileCreationTime(const std::wstring& path) {
    const auto native = ToUtf8(path);
#ifdef __APPLE__
    struct stat st{};
    if (lstat(native.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_birthtimespec.tv_sec <= 0) return 0;
    return FileTimeFromTimespec(st.st_birthtimespec);
#elif defined(STATX_BTIME)
    struct statx stx{};
    if (statx(AT_FDCWD, native.c_str(), AT_SYMLINK_NOFOLLOW, STATX_TYPE | STATX_BTIME, &stx) == 0 &&
        (stx.stx_mask & STATX_TYPE) && S_ISREG(stx.stx_mode) &&
        (stx.stx_mask & STATX_BTIME) && stx.stx_btime.tv_sec > 0) {
        timespec ts{static_cast<time_t>(stx.stx_btime.tv_sec), static_cast<long>(stx.stx_btime.tv_nsec)};
        return FileTimeFromTimespec(ts);
    }
    return 0;
#else
    (void)native;
    return 0;
#endif
}

bool EnumerateDirectory(const std::wstring& directory,
                        std::atomic<bool>& cancelFlag,
                        const std::function<void(DirectoryEntry&&)>& onEntry,
                        HRESULT* outError) {
    if (outError) *outError = S_OK;
    const auto native = ToUtf8(directory);
    DIR* dir = opendir(native.c_str());
    if (!dir) {
        if (errno == ENOENT) return true;
        if (outError) *outError = HResultFromErrno(errno);
        return false;
    }
    const int dirFd = dirfd(dir);
    errno = 0;
    while (!cancelFlag.load(std::memory_order_relaxed)) {
        const dirent* item = readdir(dir);
        if (!item) break;
        const std::string name = item->d_name;
        if (name == "." || name == "..") continue;
        struct stat st{};
        if (fstatat(dirFd, item->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) continue;
        if (IsSystemEntry(name, S_ISDIR(st.st_mode))) continue;
        DirectoryEntry entry;
        entry.fileName = FromUtf8(name);
        entry.fullPath = directory;
        if (!entry.fullPath.empty() && entry.fullPath.back() != L'/') entry.fullPath += L'/';
        entry.fullPath += entry.fileName;
        // A symbolic link is reported like a Windows reparse point; whether it names a folder follows its target.
        entry.isReparsePoint = S_ISLNK(st.st_mode);
        struct stat target{};
        entry.isDirectory = entry.isReparsePoint ? (fstatat(dirFd, item->d_name, &target, 0) == 0 && S_ISDIR(target.st_mode)) : S_ISDIR(st.st_mode);
        if (!entry.isDirectory) {
            entry.fileSize = static_cast<uint64_t>(st.st_size);
#ifdef __APPLE__
            entry.lastWriteTime = FileTimeFromTimespec(st.st_mtimespec);
            entry.createdTime = st.st_birthtimespec.tv_sec > 0 ? FileTimeFromTimespec(st.st_birthtimespec) : 0;
#else
            entry.lastWriteTime = FileTimeFromTimespec(st.st_mtim);
#endif
        }
        onEntry(std::move(entry));
        errno = 0;
    }
    const int enumerationError = errno;
    closedir(dir);
    if (!cancelFlag.load() && enumerationError != 0) {
        if (outError) *outError = HResultFromErrno(enumerationError);
        return false;
    }
    return true;
}

ReadLock::ReadLock(ReadLock&& other) noexcept : handle_(other.handle_), error_(other.error_) {
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
    // Like the Windows reparse-point flag: a symbolic link is refused rather than followed.
    const int fd = ::open(ToUtf8(path).c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        error_ = HResultFromErrno(errno);
        return false;
    }
    handle_ = reinterpret_cast<void*>(static_cast<intptr_t>(fd) + 1); // +1 keeps descriptor 0 distinct from "no handle"
    error_ = S_OK;
    return true;
}

void ReadLock::close() {
    if (handle_) {
        ::close(static_cast<int>(reinterpret_cast<intptr_t>(handle_) - 1));
        handle_ = nullptr;
    }
}

bool RevealInFileManager(const std::wstring& path) {
    const auto file = QString::fromStdWString(path);
#ifdef __APPLE__
    return QProcess::startDetached("open", {"-R", file});
#else
    // File managers that implement org.freedesktop.FileManager1 select the file; otherwise open its folder.
    // The call is made directly rather than through dbus-send, which a Flatpak runtime need not contain.
    auto call = QDBusMessage::createMethodCall("org.freedesktop.FileManager1", "/org/freedesktop/FileManager1",
        "org.freedesktop.FileManager1", "ShowItems");
    call << QStringList{QUrl::fromLocalFile(file).toString(QUrl::FullyEncoded)} << QString();
    if (QDBusConnection::sessionBus().call(call, QDBus::Block, 2000).type() == QDBusMessage::ReplyMessage) return true;
    return QProcess::startDetached("xdg-open", {QString::fromStdString(std::filesystem::path(ToUtf8(path)).parent_path().string())});
#endif
}

// The Explorer "Send To" folder exists only on Windows.
std::wstring SendToFolderPath() { return {}; }
bool IsOwnedShortcut(const std::wstring&, const wchar_t*) { return false; }
bool InstallOwnedShortcut(const std::wstring&, const std::wstring&, const std::wstring&, const wchar_t*, const wchar_t*) { return false; }

#ifndef __APPLE__
// Windows Imaging Component has no Linux counterpart; the Qt, FFmpeg and libheif readers cover these images.
// macOS uses ImageIO instead (imageio_mac.cpp).
bool DecodeWicImage(const uint8_t*, size_t, std::atomic<bool>&, HRESULT& error, WicDecodedImage&) {
    error = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    return false;
}
#endif

} // namespace Platform
