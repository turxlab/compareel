#pragma once
#include "platform/Platform.h"

// Use extended Win32 paths at API boundaries; keep ordinary paths in the UI and cache.
inline std::wstring NativeFilePath(const std::wstring& path) {
    return Platform::NativeFilePath(path);
}
