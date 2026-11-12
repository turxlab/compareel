// POSIX (Linux, macOS) file identity, hashing, trash and verified transfers.
// Mirrors file_ops_win.cpp step by step: identity checked before and after, content verified by SHA-256,
// an existing target is never overwritten, and a failed move to trash never becomes a permanent delete.
#include "platform/Platform.h"
#include "platform/PlatformFileOps.h"
#include "platform/PosixSupport.h"
#include <QFile>
#ifndef __APPLE__
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusUnixFileDescriptor>
#endif
#include <QString>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
extern "C" {
#include <libavutil/sha.h>
#include <libavutil/mem.h>
}
#ifdef __linux__
#include <sys/syscall.h>
#include <linux/fs.h>
#endif

namespace Platform {
using namespace Posix;

namespace {
struct Fd {
    int value = -1;
    explicit Fd(int fd = -1) : value(fd) {}
    ~Fd() { reset(); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    void reset(int fd = -1) { if (value >= 0) ::close(value); value = fd; }
    bool valid() const { return value >= 0; }
};

timespec ModificationTime(const struct stat& st) {
#ifdef __APPLE__
    return st.st_mtimespec;
#else
    return st.st_mtim;
#endif
}

bool ValidHash(const std::string& hash) {
    return hash.size() == 64 && hash.find_first_not_of("0123456789abcdef") == std::string::npos;
}

bool Exists(const std::string& path, int& error) {
    struct stat st{};
    if (lstat(path.c_str(), &st) == 0) return true;
    error = errno;
    return false;
}

// Rename that fails instead of replacing an existing target.
int RenameNoReplace(const std::string& from, const std::string& to) {
#if defined(__APPLE__)
    return renamex_np(from.c_str(), to.c_str(), RENAME_EXCL);
#elif defined(__linux__) && defined(SYS_renameat2)
    if (syscall(SYS_renameat2, AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), RENAME_NOREPLACE) == 0) return 0;
    if (errno != ENOSYS && errno != EINVAL) return -1;
    // Kernel or file system without RENAME_NOREPLACE: a hard link plus unlink cannot replace either.
    if (link(from.c_str(), to.c_str()) != 0) return -1;
    if (unlink(from.c_str()) != 0) { const int error = errno; unlink(to.c_str()); errno = error; return -1; }
    return 0;
#else
    if (link(from.c_str(), to.c_str()) != 0) return -1;
    if (unlink(from.c_str()) != 0) { const int error = errno; unlink(to.c_str()); errno = error; return -1; }
    return 0;
#endif
}

uint64_t DeviceOfFolder(const std::string& destination) {
    const auto slash = destination.find_last_of('/');
    const auto folder = slash == std::string::npos ? std::string(".") : slash == 0 ? std::string("/") : destination.substr(0, slash);
    struct stat st{};
    return stat(folder.c_str(), &st) == 0 ? static_cast<uint64_t>(st.st_dev) : ~0ULL;
}

// Copies into a newly created file (O_EXCL), reporting progress and honouring cancellation; keeps the modification time.
HRESULT CopyNew(int source, const std::string& destination, uint64_t total, std::atomic<bool>& cancel,
                const FileOperations::TransferProgress& progress) {
    struct stat st{};
    if (fstat(source, &st) != 0) return HResultFromErrno(errno);
    Fd target(::open(destination.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, st.st_mode & 0777));
    if (!target.valid()) return HResultFromErrno(errno);
    if (lseek(source, 0, SEEK_SET) != 0) return HResultFromErrno(errno);
    std::vector<uint8_t> buffer(1024 * 1024);
    uint64_t done = 0;
    HRESULT result = S_OK;
    for (;;) {
        if (cancel.load()) { result = HRESULT_FROM_WIN32(ERROR_CANCELLED); break; }
        const ssize_t read = ::read(source, buffer.data(), buffer.size());
        if (read < 0) { if (errno == EINTR) continue; result = HResultFromErrno(errno); break; }
        if (read == 0) break;
        for (ssize_t written = 0; written < read;) {
            const ssize_t n = ::write(target.value, buffer.data() + written, static_cast<size_t>(read - written));
            if (n < 0) { if (errno == EINTR) continue; result = HResultFromErrno(errno); break; }
            written += n;
        }
        if (FAILED(result)) break;
        done += static_cast<uint64_t>(read);
        try { if (progress) progress(done, total); } catch (...) { result = E_FAIL; break; }
    }
    if (SUCCEEDED(result) && fsync(target.value) != 0) result = HResultFromErrno(errno);
    if (SUCCEEDED(result)) {
        const timespec times[2] = {ModificationTime(st), ModificationTime(st)};
        futimens(target.value, times);
    }
    target.reset();
    if (FAILED(result)) unlink(destination.c_str());
    return result;
}
#ifndef __APPLE__
// A Flatpak sees the host's files but not the host's Trash: QFile::moveToTrash would put the file in the sandbox's
// own data folder, out of the user's sight. The Trash portal moves it to the real Trash instead.
bool InsideFlatpak() { return access("/.flatpak-info", F_OK) == 0; }

HRESULT TrashThroughPortal(const std::string& native) {
    // The portal trashes only a file the caller could change, and older portals (Ubuntu 22.04's) judge that from the
    // descriptor's access mode, refusing an O_PATH one; so it is opened for writing. Nothing is written through it.
    int fd = ::open(native.c_str(), O_RDWR | O_NOFOLLOW | O_CLOEXEC | O_NOCTTY);
    if (fd < 0) fd = ::open(native.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return HResultFromErrno(errno);
    auto call = QDBusMessage::createMethodCall("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.Trash", "TrashFile");
    call << QVariant::fromValue(QDBusUnixFileDescriptor(fd));
    ::close(fd); // QDBusUnixFileDescriptor holds its own duplicate
    const auto reply = QDBusConnection::sessionBus().call(call, QDBus::Block, 60000);
    // The portal's own reason goes to the log, so a refusal on some desktop can be diagnosed from a terminal run.
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
        qWarning("Trash portal: %s %s", qPrintable(reply.errorName()), qPrintable(reply.errorMessage()));
        return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    }
    const auto result = reply.arguments().first().toUInt();
    if (result != 1) qWarning("Trash portal refused %s (result %u)", native.c_str(), result);
    return result == 1 ? S_OK : HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
}
#endif
} // namespace

std::wstring FormatErrorMessage(HRESULT error) {
    const auto code = static_cast<uint32_t>(error);
    if ((code & 0xFFFF0000u) == 0x80070000u) {
        switch (code & 0xFFFFu) {
        case ERROR_FILE_NOT_FOUND: return L"The file could not be found.";
        case ERROR_PATH_NOT_FOUND: return L"The folder could not be found.";
        case ERROR_ACCESS_DENIED: return L"Access is denied.";
        case ERROR_WRITE_PROTECT: return L"The medium is write protected.";
        case ERROR_FILE_EXISTS: case ERROR_ALREADY_EXISTS: return L"A file with this name already exists.";
        case ERROR_DISK_FULL: return L"There is not enough space on the disk.";
        case ERROR_SHARING_VIOLATION: return L"The file is in use by another program.";
        case ERROR_CANCELLED: return L"The operation was cancelled.";
        case ERROR_CRC: return L"The file contents did not verify.";
        case ERROR_FILE_INVALID: return L"The file changed or is no longer the same file.";
        case ERROR_NOT_SUPPORTED: return L"This location does not support the operation, for example a drive without a Trash. The file was kept.";
        case ERROR_NOT_ENOUGH_MEMORY: return L"There is not enough memory to complete the operation.";
        case ERROR_DIRECTORY: return L"A folder is in the way of this file name.";
        case ERROR_INVALID_NAME: return L"The file name or path is too long or invalid.";
        case ERROR_DIR_NOT_EMPTY: return L"The folder is not empty.";
        case ERROR_SEM_TIMEOUT: return L"The drive did not respond in time.";
        case ERROR_GEN_FAILURE: return L"The drive reported an input/output error.";
        default: break;
        }
    }
    wchar_t buffer[64];
    std::swprintf(buffer, std::size(buffer), L"The file operation could not be completed (0x%08X).", code);
    return buffer;
}

FileIdentity ReadFileIdentity(const std::wstring& path, DWORD* error) {
    if (error) *error = ERROR_SUCCESS;
    struct stat st{};
    if (lstat(Posix::ToUtf8(path).c_str(), &st) != 0) {
        if (error) *error = Win32ErrorFromErrno(errno);
        return {};
    }
    // Folders and symbolic links are not files Compareel may act on, as with Windows reparse points.
    if (!S_ISREG(st.st_mode)) {
        if (error) *error = ERROR_NOT_SUPPORTED;
        return {};
    }
    return {static_cast<uint64_t>(st.st_dev), static_cast<uint64_t>(st.st_ino), static_cast<uint64_t>(st.st_size),
            FileTimeFromTimespec(ModificationTime(st)), true};
}

std::string HashFileSha256(const std::wstring& path, std::atomic<bool>& cancel, HRESULT& error, const FileIdentity& expected) {
    error = S_OK;
    const std::string utf8Path = Posix::ToUtf8(path);
    struct stat lst{};
    if (lstat(utf8Path.c_str(), &lst) != 0) { error = HResultFromErrno(errno); return {}; }
    if (!S_ISREG(lst.st_mode)) { error = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED); return {}; }
    // O_NONBLOCK prevents opening a swapped-in FIFO from blocking before fstat can reject non-regular files.
    Fd file(::open(utf8Path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (!file.valid()) {
        error = (errno == ELOOP) ? HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) : HResultFromErrno(errno);
        return {};
    }
    struct stat st{};
    if (fstat(file.value, &st) != 0) { error = HResultFromErrno(errno); return {}; }
    if (!S_ISREG(st.st_mode)) { error = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED); return {}; }
    const FileIdentity opened{
        static_cast<uint64_t>(st.st_dev),
        static_cast<uint64_t>(st.st_ino),
        static_cast<uint64_t>(st.st_size),
        FileTimeFromTimespec(ModificationTime(st)),
        true
    };
    if (expected.valid && opened != expected) { error = HRESULT_FROM_WIN32(ERROR_FILE_INVALID); return {}; }
    if (const int flags = fcntl(file.value, F_GETFL); flags >= 0 && (flags & O_NONBLOCK)) {
        (void)fcntl(file.value, F_SETFL, flags & ~O_NONBLOCK);
    }
    struct AVSHA* sha = av_sha_alloc();
    if (!sha || av_sha_init(sha, 256) < 0) { av_free(sha); error = E_OUTOFMEMORY; return {}; }
    std::vector<uint8_t> buffer(1024 * 1024);
    uint64_t totalRead = 0;
    while (!cancel.load()) {
        const ssize_t read = ::read(file.value, buffer.data(), buffer.size());
        if (read < 0) { if (errno == EINTR) continue; error = HResultFromErrno(errno); break; }
        if (read == 0) break;
        totalRead += static_cast<uint64_t>(read);
        av_sha_update(sha, buffer.data(), static_cast<size_t>(read));
    }
    if (cancel.load()) error = HRESULT_FROM_WIN32(ERROR_CANCELLED);
    if (SUCCEEDED(error) && totalRead != opened.size) error = HRESULT_FROM_WIN32(ERROR_FILE_INVALID);
    std::array<uint8_t, 32> digest{};
    if (SUCCEEDED(error)) av_sha_final(sha, digest.data());
    av_free(sha);
    if (FAILED(error)) return {};
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (auto byte : digest) { result += hex[byte >> 4]; result += hex[byte & 15]; }
    return result;
}

FileOperations::TransferResult TransferFile(
    const std::wstring& source,
    const std::wstring& destination,
    FileOperations::TransferKind kind,
    const FileIdentity& expected,
    bool allowSameVolumeRename,
    std::atomic<bool>& cancel,
    const FileOperations::TransferProgress& progress,
    const std::string& expectedHash) {
    using FileOperations::TransferKind;
    using FileOperations::TransferResult;
    TransferResult result;
    if (source.empty() || destination.empty() || !expected.valid ||
        (kind != TransferKind::Copy && kind != TransferKind::Move) ||
        (!expectedHash.empty() && !ValidHash(expectedHash))) {
        result.error = E_INVALIDARG;
        return result;
    }
    if (cancel.load()) { result.error = HRESULT_FROM_WIN32(ERROR_CANCELLED); return result; }
    const auto nativeSource = Posix::ToUtf8(source), nativeDestination = Posix::ToUtf8(destination);
    Fd sourceLock(::open(nativeSource.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
    if (!sourceLock.valid()) { result.error = HResultFromErrno(errno); return result; }
    if (ReadFileIdentity(source) != expected) { result.error = HRESULT_FROM_WIN32(ERROR_FILE_INVALID); return result; }
    // A read-only source is not moved, as on Windows.
    if (kind == TransferKind::Move && access(nativeSource.c_str(), W_OK) != 0) { result.error = HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED); return result; }
    result.contentHash = HashFileSha256(source, cancel, result.error, expected);
    if (FAILED(result.error)) return result;
    if (!expectedHash.empty() && result.contentHash != expectedHash) { result.error = HRESULT_FROM_WIN32(ERROR_CRC); return result; }
    if (kind == TransferKind::Move && allowSameVolumeRename && DeviceOfFolder(nativeDestination) == expected.volume) {
        if (cancel.load()) { result.error = HRESULT_FROM_WIN32(ERROR_CANCELLED); return result; }
        sourceLock.reset();
        if (RenameNoReplace(nativeSource, nativeDestination) != 0) {
            if (errno != EXDEV) { result.error = HResultFromErrno(errno); return result; }
        } else {
            // A rename keeps the inode, so the moved file must still be the verified source.
            result.destinationIdentity = ReadFileIdentity(destination);
            if (result.destinationIdentity != expected) {
                if (RenameNoReplace(nativeDestination, nativeSource) == 0) result.destinationIdentity = {};
                else result.copied = true;
                result.error = HRESULT_FROM_WIN32(ERROR_FILE_INVALID);
                return result;
            }
            try { if (progress) progress(expected.size, expected.size); } catch (...) {}
            result.copied = result.moved = true;
            return result;
        }
        sourceLock.reset(::open(nativeSource.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
        if (!sourceLock.valid()) { result.error = HResultFromErrno(errno); return result; }
        if (ReadFileIdentity(source) != expected) { result.error = HRESULT_FROM_WIN32(ERROR_FILE_INVALID); return result; }
    }
    // O_EXCL applies at creation, so a concurrent target is never overwritten.
    result.error = CopyNew(sourceLock.value, nativeDestination, expected.size, cancel, progress);
    if (FAILED(result.error)) return result;
    result.copied = true;
    result.destinationIdentity = ReadFileIdentity(destination);
    Fd destinationLock(::open(nativeDestination.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
    if (!destinationLock.valid()) { result.error = HResultFromErrno(errno); return result; }
    // The source is untouched until verification succeeds, so an unverified copy this call created is only clutter.
    auto discardCopy = [&](HRESULT error) {
        result.error = error;
        destinationLock.reset();
        if (result.destinationIdentity.valid && ReadFileIdentity(destination) == result.destinationIdentity && unlink(nativeDestination.c_str()) == 0) {
            result.copied = false;
            result.destinationIdentity = {};
        }
        return result;
    };
    if (!result.destinationIdentity.valid || ReadFileIdentity(destination) != result.destinationIdentity) {
        result.error = HRESULT_FROM_WIN32(ERROR_FILE_INVALID);
        return result;
    }
    if (ReadFileIdentity(source) != expected) return discardCopy(HRESULT_FROM_WIN32(ERROR_FILE_INVALID));
    HRESULT hashError = S_OK;
    const auto destinationHash = HashFileSha256(destination, cancel, hashError, result.destinationIdentity);
    if (FAILED(hashError)) return discardCopy(hashError);
    if (destinationHash != result.contentHash) return discardCopy(HRESULT_FROM_WIN32(ERROR_CRC));
    if (kind == TransferKind::Move) {
        if (cancel.load()) return discardCopy(HRESULT_FROM_WIN32(ERROR_CANCELLED));
        sourceLock.reset();
        result.error = RemoveFile(nullptr, source, true, expected);
        result.moved = SUCCEEDED(result.error);
    }
    return result;
}

FileOperations::TransferResult UndoTransferFile(
    const std::wstring& source,
    const std::wstring& destination,
    FileOperations::TransferKind kind,
    const FileIdentity& originalSource,
    const FileIdentity& createdDestination,
    bool allowSameVolumeRename,
    const std::string& hash,
    std::atomic<bool>& cancel) {
    using FileOperations::TransferKind;
    using FileOperations::TransferResult;
    TransferResult result;
    if (source.empty() || destination.empty() || !originalSource.valid || !createdDestination.valid ||
        (kind != TransferKind::Copy && kind != TransferKind::Move) || !ValidHash(hash)) {
        result.error = E_INVALIDARG;
        return result;
    }
    if (cancel.load()) { result.error = HRESULT_FROM_WIN32(ERROR_CANCELLED); return result; }
    if (kind == TransferKind::Move) {
        int missing = 0;
        if (Exists(Posix::ToUtf8(source), missing)) { result.error = HRESULT_FROM_WIN32(ERROR_FILE_EXISTS); return result; }
        if (missing != ENOENT) { result.error = HResultFromErrno(missing); return result; }
        return TransferFile(destination, source, TransferKind::Move, createdDestination, allowSameVolumeRename, cancel, {}, hash);
    }
    Fd sourceLock(::open(Posix::ToUtf8(source).c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
    if (!sourceLock.valid()) { result.error = HResultFromErrno(errno); return result; }
    if (ReadFileIdentity(source) != originalSource || ReadFileIdentity(destination) != createdDestination) {
        result.error = HRESULT_FROM_WIN32(ERROR_FILE_INVALID);
        return result;
    }
    const auto originalHash = HashFileSha256(source, cancel, result.error, originalSource);
    if (FAILED(result.error)) return result;
    if (originalHash != hash) { result.error = HRESULT_FROM_WIN32(ERROR_CRC); return result; }
    const auto destinationHash = HashFileSha256(destination, cancel, result.error, createdDestination);
    if (FAILED(result.error)) return result;
    if (destinationHash != hash) { result.error = HRESULT_FROM_WIN32(ERROR_CRC); return result; }
    if (cancel.load()) { result.error = HRESULT_FROM_WIN32(ERROR_CANCELLED); return result; }
    result.error = RemoveFile(nullptr, destination, true, createdDestination);
    return result;
}

HRESULT RemoveFile(HWND, const std::wstring& path, bool recycle, const FileIdentity& expected) {
    if (path.empty()) return E_INVALIDARG;
    DWORD error = ERROR_SUCCESS;
    const auto current = ReadFileIdentity(path, &error);
    if (!current.valid) return HRESULT_FROM_WIN32(error);
    if (expected.valid && current != expected) return HRESULT_FROM_WIN32(ERROR_FILE_INVALID);
    const auto native = Posix::ToUtf8(path);
    if (access(native.c_str(), W_OK) != 0) return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
    // The requested mode is final: a failed move to the trash must never become permanent deletion.
    if (recycle) {
        // The file is writable, so a refusal here means the folder is read-only or the volume has no trash
        // (common on network shares and some USB drives); the caller reports which.
#ifndef __APPLE__
        if (InsideFlatpak()) {
            if (const auto trashed = TrashThroughPortal(native); FAILED(trashed)) return trashed;
        } else
#endif
        if (!QFile::moveToTrash(QString::fromStdWString(path))) {
            const auto folder = std::filesystem::path(native).parent_path().string();
            return HRESULT_FROM_WIN32(access(folder.c_str(), W_OK) != 0 ? ERROR_ACCESS_DENIED : ERROR_NOT_SUPPORTED);
        }
    } else if (unlink(native.c_str()) != 0) {
        return HResultFromErrno(errno);
    }
    int missing = 0;
    if (Exists(native, missing)) return E_FAIL;
    return missing == ENOENT ? S_OK : HResultFromErrno(missing);
}

} // namespace Platform
