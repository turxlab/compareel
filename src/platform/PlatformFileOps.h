#pragma once
#include "FileOperations.h"

namespace Platform {

FileOperations::TransferResult TransferFile(
    const std::wstring& source,
    const std::wstring& destination,
    FileOperations::TransferKind kind,
    const FileIdentity& expected,
    bool allowSameVolumeRename,
    std::atomic<bool>& cancel,
    const FileOperations::TransferProgress& progress = {},
    const std::string& expectedHash = {});

FileOperations::TransferResult UndoTransferFile(
    const std::wstring& source,
    const std::wstring& destination,
    FileOperations::TransferKind kind,
    const FileIdentity& originalSource,
    const FileIdentity& createdDestination,
    bool allowSameVolumeRename,
    const std::string& contentHash,
    std::atomic<bool>& cancel);

} // namespace Platform
