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

int exifOrientation(QByteArray data);
int containerOrientation(QFile& file, bool webp, std::atomic<bool>& cancel, bool* outAnimated = nullptr);
QImage orient(QImage image, int orientation);
QImage toWhiteSRgb(QImage image);
uint8_t tonemapFloatToSrgbByte(float v);

QImage decodeWithWic(const QByteArray& bytes, std::atomic<bool>& cancel, HRESULT& error);
QImage decodeSvgSafe(const QByteArray& inputBytes, bool isSvgz, std::atomic<bool>& cancel, HRESULT& error, ImageExtendedInfo* ext);
QImage decodePacketWithFFmpegCodec(AVCodecID codecId, const QByteArray& bytes, std::atomic<bool>& cancel, HRESULT& error, bool* outIsHdr = nullptr);
bool isValidTgaHeader(const QByteArray& bytes);

QImage decodeRadianceHdrNative(const QByteArray& bytes, HRESULT& error);
QImage decodePfmNative(const QByteArray& bytes, HRESULT& error);
QImage decodePsdNative(const QByteArray& bytes, HRESULT& error);
bool isRawExtension(const QString& ext);
QImage decodeCameraRaw(const QString& extension, const QByteArray& bytes, std::atomic<bool>& cancel, HRESULT& error, bool& outAlreadyWhiteSRgb, ImageExtendedInfo* ext);

QImage decodeApng(const QByteArray& bytes, int exifOrient, std::atomic<bool>& cancel, HRESULT& error, ImageExtendedInfo& ext);
QImage decodeQtAnimated(QImageReader& reader, std::atomic<bool>& cancel, HRESULT& error, ImageExtendedInfo* ext);
QImage decodeWithQt(QFile& input, const QString& extension, const QByteArray& bytes, std::atomic<bool>& cancel, HRESULT& error, QByteArray& outFormat, bool& outAlreadyWhiteSRgb, ImageExtendedInfo* ext);

} // namespace StillImages::Internal
