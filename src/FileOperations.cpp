#include "FileOperations.h"
#include "platform/Platform.h"
#include "platform/PlatformFileOps.h"

namespace FileOperations {

std::wstring Result::message() const {
    return Platform::FormatErrorMessage(error);
}

FileIdentity ReadIdentity(const std::wstring& path, DWORD* error) {
    return Platform::ReadFileIdentity(path, error);
}

std::string ContentHash(const std::wstring& path, std::atomic<bool>& cancel, HRESULT& error) {
    return Platform::HashFileSha256(path, cancel, error);
}

TransferResult Transfer(const std::wstring& source, const std::wstring& destination, TransferKind kind,
                        const FileIdentity& expected, std::atomic<bool>& cancel,
                        const TransferProgress& progress, const std::string& expectedHash) {
    return Platform::TransferFile(source, destination, kind, expected, sameVolumeRename.load(), cancel, progress, expectedHash);
}

TransferResult UndoTransfer(const std::wstring& source, const std::wstring& destination, TransferKind kind,
                            const FileIdentity& originalSource, const FileIdentity& createdDestination,
                            const std::string& hash, std::atomic<bool>& cancel) {
    return Platform::UndoTransferFile(source, destination, kind, originalSource, createdDestination, sameVolumeRename.load(), hash, cancel);
}

Result Remove(HWND owner, const std::wstring& path, bool recycle, const FileIdentity& expected) {
    return {Platform::RemoveFile(owner, path, recycle, expected)};
}

} // namespace FileOperations
