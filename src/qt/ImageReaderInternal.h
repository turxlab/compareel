#pragma once
#include "ImageReader.h"
#include <QByteArray>
#include <QFile>
#include <QImage>
#include <QImageReader>
#include <QString>

extern "C" {
#include <libavcodec/codec_id.h>
}

namespace StillImages::Internal {

struct DdsHeaderInfo {
    bool valid = false;
    uint32_t width = 0;
    uint32_t height = 0;
    qsizetype dataOffset = 0;
    int floatChannels = 0; // 0 if not uncompressed float/half-float
    bool isHalfFloat = false;
};

int exifOrientation(QByteArray data);
int64_t exifCaptureTimeSec(QByteArray data);
int64_t jpegExifCaptureTimeSec(const QByteArray& bytes);
int containerOrientation(const QByteArray& bytes, bool webp, std::atomic<bool>& cancel, bool* outAnimated = nullptr, int64_t* outCaptureTimeSec = nullptr);
QImage orient(QImage image, int orientation);
QImage toWhiteSRgb(QImage image);
uint8_t tonemapFloatToSrgbByte(float v);

QImage decodeWithWic(const QByteArray& bytes, std::atomic<bool>& cancel, HRESULT& error);
QImage decodeSvgSafe(const QByteArray& inputBytes, bool isSvgz, std::atomic<bool>& cancel, HRESULT& error, ImageExtendedInfo* ext);
QImage decodePacketWithFFmpegCodec(AVCodecID codecId, const QByteArray& bytes, std::atomic<bool>& cancel, HRESULT& error, bool* outIsHdr = nullptr);
bool isValidTgaHeader(const QByteArray& bytes);

DdsHeaderInfo parseDdsHeader(const QByteArray& bytes);
QImage decodeDdsFloatNative(const QByteArray& bytes, const DdsHeaderInfo& hdr, HRESULT& error);
QImage decodeRadianceHdrNative(const QByteArray& bytes, HRESULT& error);
QImage decodePfmNative(const QByteArray& bytes, HRESULT& error);
QImage decodePsdNative(const QByteArray& bytes, HRESULT& error);
bool isRawExtension(const QString& ext);
int64_t rawCaptureTimeSec(const QByteArray& bytes);
QImage decodeCameraRaw(const QString& extension, const QByteArray& bytes, std::atomic<bool>& cancel, HRESULT& error, bool& outAlreadyWhiteSRgb, ImageExtendedInfo* ext);

QImage decodeApng(const QByteArray& bytes, int exifOrient, std::atomic<bool>& cancel, HRESULT& error, ImageExtendedInfo& ext);
QImage decodeQtAnimated(QImageReader& reader, std::atomic<bool>& cancel, HRESULT& error, ImageExtendedInfo* ext);
QImage decodeWithQt(const QString& extension, const QByteArray& bytes, std::atomic<bool>& cancel, HRESULT& error, QByteArray& outFormat, bool& outAlreadyWhiteSRgb, ImageExtendedInfo* ext, int precomputedOrientation = 0);

} // namespace StillImages::Internal
