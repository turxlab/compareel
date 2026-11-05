#pragma once
// Helpers shared by the POSIX (Linux, macOS) platform implementation.
#include "Common.h"
#include <cerrno>
#include <ctime>

namespace Platform::Posix {

// Paths are std::wstring throughout Compareel; on POSIX wchar_t holds UTF-32 and the file system takes UTF-8.
// A file name is any byte string on Linux, so bytes that are not valid UTF-8 are kept as U+DC80..U+DCFF
// (Python's "surrogateescape") and written back unchanged; such a file stays readable, movable and removable.
inline std::string ToUtf8(const std::wstring& text) {
    std::string out;
    out.reserve(text.size());
    for (wchar_t ch : text) {
        auto c = static_cast<uint32_t>(ch);
        if (c >= 0xDC80 && c <= 0xDCFF) { out += static_cast<char>(c - 0xDC00); continue; }
        if (c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) c = 0xFFFD;
        if (c < 0x80) out += static_cast<char>(c);
        else if (c < 0x800) { out += static_cast<char>(0xC0 | (c >> 6)); out += static_cast<char>(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { out += static_cast<char>(0xE0 | (c >> 12)); out += static_cast<char>(0x80 | ((c >> 6) & 0x3F)); out += static_cast<char>(0x80 | (c & 0x3F)); }
        else { out += static_cast<char>(0xF0 | (c >> 18)); out += static_cast<char>(0x80 | ((c >> 12) & 0x3F)); out += static_cast<char>(0x80 | ((c >> 6) & 0x3F)); out += static_cast<char>(0x80 | (c & 0x3F)); }
    }
    return out;
}

inline std::wstring FromUtf8(const std::string& text) {
    std::wstring out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        const auto b = static_cast<unsigned char>(text[i]);
        size_t length = b < 0x80 ? 1 : (b >> 5) == 0x6 ? 2 : (b >> 4) == 0xE ? 3 : (b >> 3) == 0x1E ? 4 : 0;
        uint32_t c = length == 1 ? b : length == 2 ? (b & 0x1F) : length == 3 ? (b & 0x0F) : (b & 0x07);
        bool valid = length != 0 && i + length <= text.size();
        for (size_t k = 1; valid && k < length; ++k) {
            const auto next = static_cast<unsigned char>(text[i + k]);
            if ((next & 0xC0) != 0x80) valid = false; else c = (c << 6) | (next & 0x3F);
        }
        // Overlong forms, surrogates and values past U+10FFFF are not valid UTF-8 either.
        static constexpr uint32_t minimum[] = {0, 0, 0x80, 0x800, 0x10000};
        if (valid && (c < minimum[length] || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF))) valid = false;
        if (!valid) { out += static_cast<wchar_t>(0xDC00 + b); ++i; continue; }
        out += static_cast<wchar_t>(c);
        i += length;
    }
    return out;
}

// errno mapped to the same Win32-based codes the Windows build reports, so messages and stored errors agree.
inline uint32_t Win32ErrorFromErrno(int error) {
    switch (error) {
    case ENOENT: return ERROR_FILE_NOT_FOUND;
    case ENOTDIR: return ERROR_PATH_NOT_FOUND;
    case EACCES: case EPERM: return ERROR_ACCESS_DENIED;
    case EROFS: return ERROR_WRITE_PROTECT;
    case EEXIST: return ERROR_FILE_EXISTS;
    case ENOSPC: return ERROR_DISK_FULL;
    case ENOMEM: return ERROR_NOT_ENOUGH_MEMORY;
    case EISDIR: return ERROR_DIRECTORY;
    case ENAMETOOLONG: return ERROR_INVALID_NAME;
    case ENOTEMPTY: return ERROR_DIR_NOT_EMPTY;
    case EBUSY: case ETXTBSY: return ERROR_SHARING_VIOLATION;
    case ETIMEDOUT: return ERROR_SEM_TIMEOUT;
    case EIO: return ERROR_GEN_FAILURE;
    case ELOOP: return ERROR_NOT_SUPPORTED;
    case EXDEV: return ERROR_NOT_SUPPORTED;
    case ECANCELED: return ERROR_CANCELLED;
    default: return ERROR_GEN_FAILURE;
    }
}
inline HRESULT HResultFromErrno(int error) { return HRESULT_FROM_WIN32(Win32ErrorFromErrno(error)); }

// Modification times use Windows FILETIME units (100 ns since 1601) on every platform, like FileIdentity on Windows.
inline uint64_t FileTimeFromTimespec(const timespec& time) {
    return (static_cast<uint64_t>(time.tv_sec) + 11644473600ULL) * 10000000ULL + static_cast<uint64_t>(time.tv_nsec) / 100;
}

} // namespace Platform::Posix
