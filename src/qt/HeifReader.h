#pragma once
#include "Common.h"
#include <QFile>
#include <QImage>
#include <vector>

namespace StillImages {
// Flattens transparency to opaque sRGB for comparison and display: white, or black when white would erase the whole
// picture (a white logo on a transparent background). The choice depends only on the pixels, so identical files agree.
QImage flattenTransparency(QImage image);

struct ImageExtendedInfo {
    QSize sourceSize; // Original oriented size when a large JPEG is decoded at reduced resolution.
    ImageEvidenceKind kind = ImageEvidenceKind::StaticImage;
    uint32_t frameOrPageCount = 1;
    int64_t totalDuration100ns = 0;
    int64_t captureTimeSec = 0;
    double fps = 0.0;
    int requestedIndex = -1;
    std::vector<QImage> sampledFrames;
    bool keepPlaybackFrames=false;
    bool comparisonTruncated=false;
    QImage previewFrame;
    std::vector<QImage> playbackFrames;
    std::vector<int64_t> frameDurations;
};

QImage readHeif(QFile& input, std::atomic<bool>& cancel, HRESULT& error, QByteArray& format, ImageExtendedInfo* ext = nullptr);
int64_t readHeifCaptureTimeSec(QFile& input, std::atomic<bool>& cancel);

}
