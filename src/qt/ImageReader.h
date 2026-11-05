#pragma once
#include "Common.h"
#include "HeifReader.h"
#include <QImage>

namespace StillImages {
bool supported(const std::wstring& path);
// Returns oriented sRGB pixels on white; never writes the source file.
QImage read(const VideoFileInfo& file, std::atomic<bool>& cancel, HRESULT& error, QByteArray* actualFormat = nullptr, bool allowExtended = false, ImageExtendedInfo* outExt = nullptr);
QImage readFrameOrPage(const VideoFileInfo& file, int index, std::atomic<bool>& cancel, HRESULT& error, int* outTotalCount = nullptr);
bool analyze(VideoFileInfo& file, std::atomic<bool>& cancel);
}
