#pragma once
#include "VideoScanner.h"

namespace VideoScannerInternal {
std::wstring ToLowerW(std::wstring s);
std::wstring NormalizePathKey(const std::wstring& path);
bool WithinFolder(const std::wstring& path, const std::wstring& folder);
std::wstring NormalizeExtW(std::wstring ext);
std::wstring FileStemLowerW(const std::wstring& fileName);
std::wstring FileExtLowerW(const std::wstring& fileName);
bool MatchesExtensionFilter(const std::wstring& fname, const ScanCriteria* criteria);
} // namespace VideoScannerInternal
