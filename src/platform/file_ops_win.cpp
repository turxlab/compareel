#include "platform/WinApi.h"
#include "platform/Platform.h"
#include "platform/PlatformFileOps.h"
#include <wrl/client.h>
#include <bcrypt.h>

using Microsoft::WRL::ComPtr;

namespace Platform {

std::wstring FormatErrorMessage(HRESULT error) {
    wchar_t* text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                   FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0,
                   reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::wstring result = text ? text : L"The file operation could not be completed.";
    if (text) LocalFree(text);
    return result;
}

FileIdentity ReadFileIdentity(const std::wstring& path, DWORD* error) {
    if (error) *error = ERROR_SUCCESS;
    HANDLE file = CreateFileW(NativeFilePath(path).c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (error) *error = GetLastError();
        return {};
    }
    BY_HANDLE_FILE_INFORMATION info{};
    bool ok = GetFileInformationByHandle(file, &info) != FALSE;
    DWORD lastError = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (!ok || (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
        if (error) *error = ok ? ERROR_NOT_SUPPORTED : lastError;
        return {};
    }
    return {info.dwVolumeSerialNumber,
        (uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow,
        (uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow,
        (uint64_t(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime,
        true};
}

std::string HashFileSha256(const std::wstring& path, std::atomic<bool>& cancel, HRESULT& error, const FileIdentity& expected) {
    error = S_OK;
    HANDLE file = CreateFileW(NativeFilePath(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) { error = HRESULT_FROM_WIN32(GetLastError()); return {}; }
    if (GetFileType(file) != FILE_TYPE_DISK) {
        CloseHandle(file);
        error = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        return {};
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file, &info)) {
        error = HRESULT_FROM_WIN32(GetLastError());
        CloseHandle(file);
        return {};
    }
    if (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) {
        CloseHandle(file);
        error = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        return {};
    }
    const FileIdentity opened{
        info.dwVolumeSerialNumber,
        (uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow,
        (uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow,
        (uint64_t(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime,
        true
    };
    if (expected.valid && opened != expected) {
        CloseHandle(file);
        error = HRESULT_FROM_WIN32(ERROR_FILE_INVALID);
        return {};
    }
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (status >= 0) status = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0);
    std::vector<uint8_t> buffer(1024 * 1024);
    uint64_t totalRead = 0;
    while (status >= 0 && !cancel.load()) {
        DWORD bytes = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes, nullptr)) {
            error = HRESULT_FROM_WIN32(GetLastError()); break;
        }
        if (!bytes) break;
        totalRead += bytes;
        status = BCryptHashData(hash, buffer.data(), bytes, 0);
    }
    std::array<uint8_t, 32> digest{};
    if (cancel.load()) error = HRESULT_FROM_WIN32(ERROR_CANCELLED);
    if (SUCCEEDED(error) && totalRead != opened.size) error = HRESULT_FROM_WIN32(ERROR_FILE_INVALID);
    if (status >= 0 && SUCCEEDED(error)) status = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
    if (status < 0) error = HRESULT_FROM_NT(status);
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(file);
    if (FAILED(error)) return {};
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (auto byte : digest) { result += hex[byte >> 4]; result += hex[byte & 15]; }
    return result;
}

namespace {
struct CopyContext {
    std::atomic<bool>& cancel;
    const FileOperations::TransferProgress& progress;
    bool callbackFailed = false;
};
DWORD CALLBACK CopyProgress(LARGE_INTEGER total, LARGE_INTEGER completed, LARGE_INTEGER, LARGE_INTEGER,
                            DWORD, DWORD, HANDLE, HANDLE, LPVOID data) {
    auto& context = *static_cast<CopyContext*>(data);
    try { if (context.progress) context.progress(uint64_t(completed.QuadPart), uint64_t(total.QuadPart)); }
    catch (...) { context.callbackFailed = true; return PROGRESS_CANCEL; }
    return context.cancel.load() ? PROGRESS_CANCEL : PROGRESS_CONTINUE;
}
bool ValidHash(const std::string& hash) {
    return hash.size() == 64 && hash.find_first_not_of("0123456789abcdef") == std::string::npos;
}
// A same-volume move is a rename: no second copy, so it needs no free space and no write verification.
bool OnVolume(const std::wstring& destination, uint64_t volume) {
    const auto slash = destination.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return false;
    auto folder = destination.substr(0, slash);
    if (folder.size() <= 2 || folder.back() == L':') folder += L'\\';
    HANDLE raw = CreateFileW(NativeFilePath(folder).c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (raw == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION info{};
    const bool ok = GetFileInformationByHandle(raw, &info) != FALSE;
    CloseHandle(raw);
    return ok && info.dwVolumeSerialNumber == volume;
}
} // namespace

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
    const auto nativeSource = NativeFilePath(source), nativeDestination = NativeFilePath(destination);
    HANDLE raw = CreateFileW(nativeSource.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (raw == INVALID_HANDLE_VALUE) { result.error = HRESULT_FROM_WIN32(GetLastError()); return result; }
    std::unique_ptr<void, decltype(&CloseHandle)> sourceLock(raw, CloseHandle);
    if (ReadFileIdentity(source) != expected) { result.error = HRESULT_FROM_WIN32(ERROR_FILE_INVALID); return result; }
    const auto attributes = GetFileAttributesW(nativeSource.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) { result.error = HRESULT_FROM_WIN32(GetLastError()); return result; }
    if (kind == TransferKind::Move && (attributes & FILE_ATTRIBUTE_READONLY)) {
        result.error = HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
        return result;
    }
    result.contentHash = HashFileSha256(source, cancel, result.error, expected);
    if (FAILED(result.error)) return result;
    if (!expectedHash.empty() && result.contentHash != expectedHash) {
        result.error = HRESULT_FROM_WIN32(ERROR_CRC);
        return result;
    }
    if (kind == TransferKind::Move && allowSameVolumeRename && OnVolume(destination, expected.volume)) {
        if (cancel.load()) { result.error = HRESULT_FROM_WIN32(ERROR_CANCELLED); return result; }
        sourceLock.reset();
        // No REPLACE_EXISTING: an existing target is never overwritten.
        if (!MoveFileExW(nativeSource.c_str(), nativeDestination.c_str(), MOVEFILE_WRITE_THROUGH)) {
            const DWORD error = GetLastError();
            if (error != ERROR_NOT_SAME_DEVICE) { result.error = HRESULT_FROM_WIN32(error); return result; }
        } else {
            // A rename keeps the file index, so the moved file must still be the verified source.
            result.destinationIdentity = ReadFileIdentity(destination);
            if (result.destinationIdentity != expected) {
                if (MoveFileExW(nativeDestination.c_str(), nativeSource.c_str(), MOVEFILE_WRITE_THROUGH)) result.destinationIdentity = {};
                else result.copied = true;
                result.error = HRESULT_FROM_WIN32(ERROR_FILE_INVALID);
                return result;
            }
            try { if (progress) progress(expected.size, expected.size); } catch (...) {}
            result.copied = result.moved = true;
            return result;
        }
        raw = CreateFileW(nativeSource.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                          FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (raw == INVALID_HANDLE_VALUE) { result.error = HRESULT_FROM_WIN32(GetLastError()); return result; }
        sourceLock.reset(raw);
        if (ReadFileIdentity(source) != expected) { result.error = HRESULT_FROM_WIN32(ERROR_FILE_INVALID); return result; }
    }
    CopyContext context{cancel, progress};
    // FAIL_IF_EXISTS applies at creation, so a concurrent target is never overwritten.
    if (!CopyFileExW(nativeSource.c_str(), nativeDestination.c_str(), CopyProgress, &context, nullptr, COPY_FILE_FAIL_IF_EXISTS)) {
        result.error = context.callbackFailed ? E_FAIL : HRESULT_FROM_WIN32(GetLastError());
        return result;
    }
    result.copied = true;
    result.destinationIdentity = ReadFileIdentity(destination);
    raw = CreateFileW(nativeDestination.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (raw == INVALID_HANDLE_VALUE) { result.error = HRESULT_FROM_WIN32(GetLastError()); return result; }
    std::unique_ptr<void, decltype(&CloseHandle)> destinationLock(raw, CloseHandle);
    // The source is untouched until verification succeeds, so an unverified copy this call created is only clutter.
    auto discardCopy = [&](HRESULT error) {
        result.error = error;
        destinationLock.reset();
        if (result.destinationIdentity.valid && ReadFileIdentity(destination) == result.destinationIdentity && DeleteFileW(nativeDestination.c_str())) {
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
        const auto attributes = GetFileAttributesW(NativeFilePath(source).c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES) { result.error = HRESULT_FROM_WIN32(ERROR_FILE_EXISTS); return result; }
        const auto missing = GetLastError();
        if (missing != ERROR_FILE_NOT_FOUND && missing != ERROR_PATH_NOT_FOUND) {
            result.error = HRESULT_FROM_WIN32(missing);
            return result;
        }
        return TransferFile(destination, source, TransferKind::Move, createdDestination, allowSameVolumeRename, cancel, {}, hash);
    }
    HANDLE raw = CreateFileW(NativeFilePath(source).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (raw == INVALID_HANDLE_VALUE) { result.error = HRESULT_FROM_WIN32(GetLastError()); return result; }
    std::unique_ptr<void, decltype(&CloseHandle)> sourceLock(raw, CloseHandle);
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

HRESULT RemoveFile(HWND owner, const std::wstring& path, bool recycle, const FileIdentity& expected) {
    if (path.empty()) return E_INVALIDARG;
    DWORD error = ERROR_SUCCESS;
    auto current = ReadFileIdentity(path, &error);
    if (!current.valid) return HRESULT_FROM_WIN32(error);
    if (expected.valid && current != expected) return HRESULT_FROM_WIN32(ERROR_FILE_INVALID);
    const auto native = NativeFilePath(path);
    DWORD attributes = GetFileAttributesW(native.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return HRESULT_FROM_WIN32(GetLastError());
    if (attributes & FILE_ATTRIBUTE_READONLY) return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);

    ComPtr<IFileOperation> operation;
    HRESULT hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&operation));
    if (FAILED(hr)) return hr;
    DWORD flags = FOF_SILENT | FOF_NOERRORUI | FOF_NOCONFIRMATION | FOFX_EARLYFAILURE;
    if (recycle) flags |= FOFX_RECYCLEONDELETE | FOFX_ADDUNDORECORD;
    if (FAILED(hr = operation->SetOperationFlags(flags))) return hr;
    if (owner && FAILED(hr = operation->SetOwnerWindow(owner))) return hr;
    ComPtr<IShellItem> item;
    auto shellPath = path;
    std::replace(shellPath.begin(), shellPath.end(), L'/', L'\\');
    if (FAILED(hr = SHCreateItemFromParsingName(shellPath.c_str(), nullptr, IID_PPV_ARGS(&item)))) return hr;
    if (FAILED(hr = operation->DeleteItem(item.Get(), nullptr))) return hr;
    // The requested mode is final: a failed recycle must never become permanent deletion.
    if (FAILED(hr = operation->PerformOperations())) return hr;
    BOOL aborted = FALSE;
    if (FAILED(hr = operation->GetAnyOperationsAborted(&aborted))) return hr;
    if (aborted) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    DWORD finalAttributes = GetFileAttributesW(native.c_str());
    if (finalAttributes != INVALID_FILE_ATTRIBUTES) return E_FAIL;
    DWORD finalError = GetLastError();
    if (finalError != ERROR_FILE_NOT_FOUND && finalError != ERROR_PATH_NOT_FOUND)
        return HRESULT_FROM_WIN32(finalError);
    return S_OK;
}

} // namespace Platform
