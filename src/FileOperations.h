#pragma once
#include "Common.h"

namespace FileOperations {
struct Result {
    HRESULT error = S_OK;
    bool succeeded() const { return SUCCEEDED(error); }
    std::wstring message() const;
};

FileIdentity ReadIdentity(const std::wstring& path, DWORD* error = nullptr);
std::string ContentHash(const std::wstring& path, std::atomic<bool>& cancel, HRESULT& error, const FileIdentity& expected = {});
Result Remove(HWND owner, const std::wstring& path, bool recycle,
              const FileIdentity& expected = {});
enum class TransferKind { Copy, Move };
struct TransferResult : Result {
    FileIdentity destinationIdentity;
    std::string contentHash;
    bool copied=false, moved=false;
};
// Same-volume moves are renames; tests clear this to exercise the cross-volume copy path on one disk.
inline std::atomic<bool> sameVolumeRename{true};
using TransferProgress=std::function<void(uint64_t completed,uint64_t total)>;
TransferResult Transfer(const std::wstring& source,const std::wstring& destination,TransferKind kind,
                        const FileIdentity& expected,std::atomic<bool>& cancel,
                        const TransferProgress& progress={},const std::string& expectedHash={});
TransferResult UndoTransfer(const std::wstring& source,const std::wstring& destination,TransferKind kind,
                            const FileIdentity& originalSource,const FileIdentity& createdDestination,
                            const std::string& contentHash,std::atomic<bool>& cancel);
}
