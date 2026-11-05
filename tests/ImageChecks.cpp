#include "ImageReader.h"
#include "FileOperations.h"
#include "PerceptualHash.h"
#include "VideoScanner.h"
#include "ScanCache.h"
#include "platform/Platform.h"
#include <QBuffer>
#include <QColorSpace>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QPainter>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTransform>
#include <QtEndian>
#ifdef _WIN32
#include <wincodec.h>
#endif
#include <stdexcept>
#include <cmath>

static void require(bool value,const char* reason) { if(!value) throw std::runtime_error(reason); }
static VideoFileInfo info(const QString& path) {
    VideoFileInfo file;
    const QFileInfo fi(path);
    file.fullPath=QDir::toNativeSeparators(fi.absoluteFilePath()).toStdWString();
    file.fileName=fi.fileName().toStdWString();
    file.folderPath=QDir::toNativeSeparators(fi.absolutePath()).toStdWString();
    file.identity=FileOperations::ReadIdentity(file.fullPath);
    file.fileSize=file.identity.size;
    return file;
}
static void write(const QString& path,const QByteArray& bytes) {
    QFile file(path); require(file.open(QIODevice::WriteOnly|QIODevice::Truncate) && file.write(bytes)==bytes.size(),"fixture write");
}
static QByteArray encode(const QImage& image,const char* format) {
    QByteArray bytes; QBuffer buffer(&bytes); require(buffer.open(QIODevice::WriteOnly),"buffer");
    QImageWriter writer(&buffer,format); writer.setQuality(100); require(writer.write(image),"fixture encode/plugin"); return bytes;
}
static QByteArray exif(int orientation,bool little) {
    auto data=QByteArray::fromHex(little?"49492a0008000000010012010300010000000100000000000000":"4d4d002a00000008000101120003000000010001000000000000");
    if(little) qToLittleEndian<quint16>(orientation,data.data()+18); else qToBigEndian<quint16>(orientation,data.data()+18);
    return data;
}
static QByteArray pngChunk(const char* type,const QByteArray& payload) {
    const QByteArray data=QByteArray(type,4)+payload; quint32 crc=0xffffffff;
    for(uchar byte:data) { crc^=byte; for(int bit=0;bit<8;++bit) crc=(crc>>1)^((crc&1)?0xedb88320:0); }
    QByteArray result(4,'\0'); qToBigEndian<quint32>(quint32(payload.size()),result.data()); result+=data;
    char tail[4]; qToBigEndian<quint32>(crc^0xffffffff,tail); result.append(tail,4); return result;
}

static QByteArray buildSyntheticPsd(const QImage& img) {
    const int w=img.width(), h=img.height();
    QByteArray psd;
    psd.append("8BPS",4);
    char b2[2], b4[4];
    qToBigEndian<quint16>(1,b2); psd.append(b2,2); // version 1
    psd.append(QByteArray(6,'\0')); // reserved
    qToBigEndian<quint16>(3,b2); psd.append(b2,2); // 3 channels (RGB)
    qToBigEndian<quint32>(h,b4); psd.append(b4,4);
    qToBigEndian<quint32>(w,b4); psd.append(b4,4);
    qToBigEndian<quint16>(8,b2); psd.append(b2,2); // 8-bit depth
    qToBigEndian<quint16>(3,b2); psd.append(b2,2); // RGB color mode (3)
    qToBigEndian<quint32>(0,b4); psd.append(b4,4); // Color Mode Data length
    qToBigEndian<quint32>(0,b4); psd.append(b4,4); // Image Resources length
    qToBigEndian<quint32>(0,b4); psd.append(b4,4); // Layer and Mask Info length
    qToBigEndian<quint16>(0,b2); psd.append(b2,2); // Compression = 0 (Raw planar)
    for(int c=0;c<3;++c) {
        for(int y=0;y<h;++y) {
            for(int x=0;x<w;++x) {
                const QRgb px=img.pixel(x,y);
                const uchar val=(c==0)?qRed(px):(c==1)?qGreen(px):qBlue(px);
                psd.append(char(val));
            }
        }
    }
    return psd;
}

static QByteArray buildMultiSizeIco(const QImage& smallImg, const QImage& largeImg) {
    const QByteArray pngSmall=encode(smallImg,"png");
    const QByteArray pngLarge=encode(largeImg,"png");
    QByteArray ico(6 + 16*2, '\0');
    qToLittleEndian<quint16>(0, ico.data()+0);
    qToLittleEndian<quint16>(1, ico.data()+2); // type 1 = ICO
    qToLittleEndian<quint16>(2, ico.data()+4); // count = 2
    const quint32 off1 = 6 + 32;
    const quint32 off2 = off1 + quint32(pngSmall.size());
    // Entry 0: small
    ico[6+0] = char(smallImg.width());
    ico[6+1] = char(smallImg.height());
    qToLittleEndian<quint16>(1, ico.data()+6+4);
    qToLittleEndian<quint16>(32, ico.data()+6+6);
    qToLittleEndian<quint32>(quint32(pngSmall.size()), ico.data()+6+8);
    qToLittleEndian<quint32>(off1, ico.data()+6+12);
    // Entry 1: large
    ico[22+0] = char(largeImg.width());
    ico[22+1] = char(largeImg.height());
    qToLittleEndian<quint16>(1, ico.data()+22+4);
    qToLittleEndian<quint16>(32, ico.data()+22+6);
    qToLittleEndian<quint32>(quint32(pngLarge.size()), ico.data()+22+8);
    qToLittleEndian<quint32>(off2, ico.data()+22+12);
    ico += pngSmall;
    ico += pngLarge;
    return ico;
}

static QByteArray buildMultiPageTiff(const QList<QImage>& pages) {
    // Build a valid little-endian uncompressed RGB multi-page TIFF
    QByteArray tiff("II*\0\x08\0\0\0", 8);
    for(int p=0; p<pages.size(); ++p) {
        const QImage img = pages[p].convertedTo(QImage::Format_RGB888);
        const quint32 w = img.width(), h = img.height();
        QByteArray pixels;
        pixels.reserve(w*h*3);
        for(quint32 y=0; y<h; ++y) {
            pixels.append(reinterpret_cast<const char*>(img.constScanLine(y)), w*3);
        }
        const quint32 ifdOffset = quint32(tiff.size());
        // Patch previous next-IFD pointer
        if(p==0) qToLittleEndian<quint32>(ifdOffset, tiff.data()+4);
        else {
            // We stored the offset of the previous next-IFD pointer in the loop
        }
        // 10 IFD entries:
        // 256 ImageWidth (LONG 1)
        // 257 ImageLength (LONG 1)
        // 258 BitsPerSample (SHORT 3 -> offset)
        // 259 Compression (SHORT 1 = 1)
        // 262 PhotometricInterpretation (SHORT 1 = 2 RGB)
        // 273 StripOffsets (LONG 1 -> pixelOffset)
        // 277 SamplesPerPixel (SHORT 1 = 3)
        // 278 RowsPerStrip (LONG 1 = h)
        // 279 StripByteCounts (LONG 1 = w*h*3)
        // 284 PlanarConfiguration (SHORT 1 = 1)
        const quint16 entryCount = 10;
        const quint32 ifdSize = 2 + entryCount*12 + 4;
        const quint32 bpsOffset = ifdOffset + ifdSize;
        const quint32 pixelOffset = bpsOffset + 6;
        const quint32 nextIfdOffset = (p + 1 < pages.size()) ? (pixelOffset + quint32(pixels.size())) : 0;

        QByteArray ifd(ifdSize, '\0');
        qToLittleEndian<quint16>(entryCount, ifd.data());
        auto setEntry = [&](int idx, quint16 tag, quint16 type, quint32 count, quint32 val) {
            char* e = ifd.data() + 2 + idx*12;
            qToLittleEndian<quint16>(tag, e);
            qToLittleEndian<quint16>(type, e+2);
            qToLittleEndian<quint32>(count, e+4);
            if(type==3 && count==1) qToLittleEndian<quint16>(quint16(val), e+8);
            else qToLittleEndian<quint32>(val, e+8);
        };
        setEntry(0, 256, 4, 1, w);
        setEntry(1, 257, 4, 1, h);
        setEntry(2, 258, 3, 3, bpsOffset);
        setEntry(3, 259, 3, 1, 1);
        setEntry(4, 262, 3, 1, 2);
        setEntry(5, 273, 4, 1, pixelOffset);
        setEntry(6, 277, 3, 1, 3);
        setEntry(7, 278, 4, 1, h);
        setEntry(8, 279, 4, 1, quint32(pixels.size()));
        setEntry(9, 284, 3, 1, 1);
        qToLittleEndian<quint32>(nextIfdOffset, ifd.data() + 2 + entryCount*12);

        QByteArray bps(6, '\0');
        qToLittleEndian<quint16>(8, bps.data()+0);
        qToLittleEndian<quint16>(8, bps.data()+2);
        qToLittleEndian<quint16>(8, bps.data()+4);

        tiff += ifd;
        tiff += bps;
        tiff += pixels;
    }
    return tiff;
}

static QByteArray buildSyntheticApng(const QList<QImage>& frames) {
    // Build a valid APNG from QImages using PNG chunks (IHDR, acTL, fcTL, IDAT, fdAT, IEND)
    const QByteArray basePng = encode(frames[0], "png");
    // Find IHDR end (offset 33) and IDAT chunks in basePng
    QByteArray ihdr = basePng.sliced(8, 25);
    QByteArray firstIdatPayload;
    int pos = 33;
    while(pos + 12 <= basePng.size()) {
        const quint32 len = qFromBigEndian<quint32>(basePng.constData() + pos);
        const QByteArray type = basePng.sliced(pos + 4, 4);
        if(type == "IDAT") firstIdatPayload += basePng.sliced(pos + 8, len);
        pos += 12 + int(len);
    }

    QByteArray apng = QByteArray::fromHex("89504e470d0a1a0a") + ihdr;
    QByteArray actl(8, '\0');
    qToBigEndian<quint32>(quint32(frames.size()), actl.data() + 0);
    qToBigEndian<quint32>(0, actl.data() + 4);
    apng += pngChunk("acTL", actl);

    quint32 seq = 0;
    for(int i=0; i<frames.size(); ++i) {
        QByteArray fctl(26, '\0');
        qToBigEndian<quint32>(seq++, fctl.data() + 0);
        qToBigEndian<quint32>(quint32(frames[i].width()), fctl.data() + 4);
        qToBigEndian<quint32>(quint32(frames[i].height()), fctl.data() + 8);
        qToBigEndian<quint32>(0, fctl.data() + 12);
        qToBigEndian<quint32>(0, fctl.data() + 16);
        qToBigEndian<quint16>(10, fctl.data() + 20); // 10/100 s = 100ms
        qToBigEndian<quint16>(100, fctl.data() + 22);
        fctl[24] = 0; // APNG_DISPOSE_OP_NONE
        fctl[25] = 0; // APNG_BLEND_OP_SOURCE
        apng += pngChunk("fcTL", fctl);

        if(i == 0) {
            apng += pngChunk("IDAT", firstIdatPayload);
        } else {
            const QByteArray framePng = encode(frames[i], "png");
            QByteArray idatPayload;
            int p = 33;
            while(p + 12 <= framePng.size()) {
                const quint32 len = qFromBigEndian<quint32>(framePng.constData() + p);
                const QByteArray type = framePng.sliced(p + 4, 4);
                if(type == "IDAT") idatPayload += framePng.sliced(p + 8, len);
                p += 12 + int(len);
            }
            QByteArray fdat(4, '\0');
            qToBigEndian<quint32>(seq++, fdat.data());
            fdat += idatPayload;
            apng += pngChunk("fdAT", fdat);
        }
    }
    apng += pngChunk("IEND", {});
    return apng;
}

static QByteArray buildSyntheticDngWithPreview(const QByteArray& jpegBytes) {
    // Minimal TIFF header + IFD with JPEGInterchangeFormat (0x0201), JPEGInterchangeFormatLength (0x0202), and DNGVersion (0xC612)
    QByteArray dng("II*\0\x08\0\0\0", 8);
    const quint16 entryCount = 3;
    const quint32 ifdSize = 2 + entryCount * 12 + 4;
    const quint32 jpgOffset = 8 + ifdSize;
    QByteArray ifd(ifdSize, '\0');
    qToLittleEndian<quint16>(entryCount, ifd.data());
    auto setTag = [&](int idx, quint16 tag, quint16 type, quint32 count, quint32 val) {
        char* e = ifd.data() + 2 + idx * 12;
        qToLittleEndian<quint16>(tag, e);
        qToLittleEndian<quint16>(type, e + 2);
        qToLittleEndian<quint32>(count, e + 4);
        qToLittleEndian<quint32>(val, e + 8);
    };
    setTag(0, 0x0201, 4, 1, jpgOffset);
    setTag(1, 0x0202, 4, 1, quint32(jpegBytes.size()));
    setTag(2, 0xC612, 1, 4, 0x00000401u);
    dng += ifd;
    dng += jpegBytes;
    return dng;
}

static QByteArray buildSyntheticPfm(const QImage& img) {
    const int w = img.width(), h = img.height();
    QByteArray pfm = QString("PF\n%1 %2\n-1.0\n").arg(w).arg(h).toLatin1();
    for(int y = h - 1; y >= 0; --y) {
        for(int x = 0; x < w; ++x) {
            const QRgb px = img.pixel(x, y);
            const float rgb[3] = { qRed(px)/255.0f, qGreen(px)/255.0f, qBlue(px)/255.0f };
            pfm.append(reinterpret_cast<const char*>(rgb), 12);
        }
    }
    return pfm;
}

#ifdef _WIN32
// JPEG XR fixtures can only be written, and read, by Windows Imaging Component.
static bool writeWicJxr(const QString& path, const QImage& img) {
    IWICImagingFactory* factory = nullptr;
    if(FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) return false;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    bool ok = false;
    if(SUCCEEDED(factory->CreateStream(&stream)) &&
       SUCCEEDED(stream->InitializeFromFilename(path.toStdWString().c_str(), GENERIC_WRITE)) &&
       SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatWmp, nullptr, &encoder)) &&
       SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) &&
       SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) &&
       SUCCEEDED(frame->Initialize(nullptr)) &&
       SUCCEEDED(frame->SetSize(img.width(), img.height()))) {
        WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
        const QImage bgra = img.convertedTo(QImage::Format_ARGB32);
        if(SUCCEEDED(frame->SetPixelFormat(&pf)) &&
           SUCCEEDED(frame->WritePixels(bgra.height(), bgra.bytesPerLine(), bgra.sizeInBytes(), const_cast<uchar*>(bgra.constBits()))) &&
           SUCCEEDED(frame->Commit()) &&
           SUCCEEDED(encoder->Commit())) {
            ok = true;
        }
    }
    if(frame) frame->Release();
    if(encoder) encoder->Release();
    if(stream) stream->Release();
    factory->Release();
    return ok;
}
#endif

int main(int argc,char** argv) {
    Platform::ComInit com(Platform::ComApartment::MultiThreaded); // released when main returns
    QCoreApplication app(argc,argv); QImageReader::setAllocationLimit(256);
    try {
        QTemporaryDir directory; require(directory.isValid(),"private fixture root");
        std::atomic<bool> cancel{false}; HRESULT error=S_OK;
        QImage pattern(128,96,QImage::Format_ARGB32); pattern.setColorSpace(QColorSpace::SRgb);
        for(int y=0;y<pattern.height();++y) for(int x=0;x<pattern.width();++x) pattern.setPixel(x,y,qRgba(x*2,y*2,(x+y)%256,255));
        { QPainter painter(&pattern); painter.fillRect(0,0,37,31,Qt::red); painter.fillRect(83,63,45,33,Qt::blue); }
        const auto expected=pattern.convertedTo(QImage::Format_RGB32);
        QByteArray jpeg,webp,png;
        for(const auto format:{"png","jpeg","webp"}) {
            const auto bytes=encode(pattern,format); const auto path=directory.filePath(QString("pattern.%1").arg(format)); write(path,bytes);
            auto file=info(path); const auto original=file.identity;
            const auto pixels=StillImages::read(file,cancel,error); require(SUCCEEDED(error) && pixels.size()==pattern.size(),"actual still decode");
            require(StillImages::analyze(file,cancel) && file.sigFirst.valid && file.sigFirst.normalized.size()==8 && file.thumbBGRA.size()==THUMB_W*THUMB_H*4,"image evidence/thumbnail");
            const auto reference=PerceptualHash::ComputeSignatureFromBGRA(expected.constBits(),128,96,expected.bytesPerLine());
            require(PerceptualHash::CompareSignatures(reference,file.sigFirst)>98,"normalized image evidence");
            require(FileOperations::ReadIdentity(file.fullPath)==original,"reader modified source");
            if(QByteArray(format)=="jpeg") jpeg=bytes; if(QByteArray(format)=="webp") webp=bytes;
            if(QByteArray(format)=="png") png=bytes;
        }
        for(int orientation=1;orientation<=8;++orientation) {
            const auto metadata=exif(orientation,orientation%2==0);
            QByteArray app1=QByteArray::fromHex("ffe1"); const auto payload=QByteArray("Exif\0\0",6)+metadata;
            char length[2]; qToBigEndian<quint16>(quint16(payload.size()+2),length); app1.append(length,2); app1+=payload;
            const auto jpegPath=directory.filePath(QString("oriented-%1.jpg").arg(orientation)); write(jpegPath,jpeg.first(2)+app1+jpeg.sliced(2));
            QImageReader referenceReader(jpegPath); referenceReader.setAutoTransform(true); const auto reference=referenceReader.read(); require(!reference.isNull(),"orientation reference");
            auto file=info(jpegPath); const auto decoded=StillImages::read(file,cancel,error);
            require(SUCCEEDED(error) && decoded.size()==reference.size() && decoded.pixelColor(10,10)==reference.pixelColor(10,10),"JPEG EXIF orientation");
            auto orientedWebp=webp;
            // The tagged-sRGB writer emits VP8X, so mark EXIF and append the bounded TIFF chunk.
            require(orientedWebp.sliced(12,4)=="VP8X","WebP extended fixture"); orientedWebp[20]=char(uchar(orientedWebp[20])|0x08);
            orientedWebp+="EXIF"; char count[4]; qToLittleEndian<quint32>(quint32(metadata.size()),count); orientedWebp.append(count,4); orientedWebp+=metadata;
            if(metadata.size()&1) orientedWebp+='\0'; qToLittleEndian<quint32>(quint32(orientedWebp.size()-8),orientedWebp.data()+4);
            const auto webpPath=directory.filePath(QString("oriented-%1.webp").arg(orientation)); write(webpPath,orientedWebp);
            file=info(webpPath); const auto webpPixels=StillImages::read(file,cancel,error);
            require(SUCCEEDED(error) && webpPixels.size()==reference.size(),"WebP EXIF size");
            const auto a=PerceptualHash::ComputeSignatureFromBGRA(decoded.constBits(),decoded.width(),decoded.height(),decoded.bytesPerLine());
            const auto b=PerceptualHash::ComputeSignatureFromBGRA(webpPixels.constBits(),webpPixels.width(),webpPixels.height(),webpPixels.bytesPerLine());
            require(PerceptualHash::CompareSignatures(a,b)>98,"WebP mirrored/rotated orientation matches JPEG");
            const auto pngPath=directory.filePath(QString("oriented-%1.png").arg(orientation));
            write(pngPath,png.first(png.size()-12)+pngChunk("eXIf",metadata)+png.last(12));
            file=info(pngPath); const auto pngPixels=StillImages::read(file,cancel,error);
            require(SUCCEEDED(error) && pngPixels.size()==reference.size(),"PNG EXIF size");
            const auto c=PerceptualHash::ComputeSignatureFromBGRA(pngPixels.constBits(),pngPixels.width(),pngPixels.height(),pngPixels.bytesPerLine());
            require(PerceptualHash::CompareSignatures(a,c)>98,"PNG mirrored/rotated orientation matches JPEG");
        }
        QImage alpha(16,16,QImage::Format_ARGB32); alpha.fill(qRgba(240,10,90,0));
        const auto alphaPath=directory.filePath("alpha.png"); write(alphaPath,encode(alpha,"png"));
        auto file=info(alphaPath); const auto white=StillImages::read(file,cancel,error); require(SUCCEEDED(error) && white.pixelColor(8,8)==Qt::white,"transparent RGB must not become visible evidence");
        auto adobe=pattern.convertedToColorSpace(QColorSpace::AdobeRgb); const auto iccPath=directory.filePath("icc.png"); write(iccPath,encode(adobe,"png"));
        file=info(iccPath); const auto srgb=StillImages::read(file,cancel,error); require(SUCCEEDED(error) && srgb.colorSpace()==QColorSpace(QColorSpace::SRgb),"ICC to sRGB");
        require(std::abs(srgb.pixelColor(60,40).red()-pattern.pixelColor(60,40).red())<3,"ICC visual value");
        file=info(alphaPath); file.identity.size++; require(StillImages::read(file,cancel,error).isNull() && error==HRESULT_FROM_WIN32(ERROR_FILE_INVALID),"changed source guard");
        file=info(alphaPath); cancel=true; require(StillImages::read(file,cancel,error).isNull() && error==HRESULT_FROM_WIN32(ERROR_CANCELLED),"cancel guard"); cancel=false;
        const auto broken=directory.filePath("broken.png"); write(broken,"invalid image bytes"); file=info(broken); require(!StillImages::analyze(file,cancel) && FAILED(file.error),"corrupt source refusal");
        const auto unsupported=directory.filePath("unsupported.gif"); write(unsupported,"GIF89a"); file=info(unsupported); require(StillImages::read(file,cancel,error).isNull() && error==HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED),"unsupported format refusal");
        const auto badExif=directory.filePath("bad-exif.png");
        write(badExif,png.first(png.size()-12)+pngChunk("eXIf",QByteArray::fromHex("49492a00ffffffff"))+png.last(12));
        file=info(badExif); require(StillImages::read(file,cancel,error).isNull() && FAILED(error),"malformed EXIF offset refusal");
        const auto animated=directory.filePath("animated.png"); QByteArray animation(8,'\0'); qToBigEndian<quint32>(2,animation.data());
        write(animated,png.first(33)+pngChunk("acTL",animation)+png.sliced(33)); file=info(animated);
        require(StillImages::read(file,cancel,error).isNull() && error==HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED),"APNG must not silently compare only its first frame");
        const auto oversized=directory.filePath("oversized.png"); auto ihdr=png.sliced(16,13); qToBigEndian<quint32>(32769,ihdr.data());
        write(oversized,png.first(8)+pngChunk("IHDR",ihdr)+png.sliced(33)); file=info(oversized);
        require(StillImages::read(file,cancel,error).isNull() && FAILED(error),"oversized image must not allocate pixels");

        // --- NEW TIER 1: Common Static Images, Aliases, Content Sniffing & Safe SVG ---
        const auto referenceSig=PerceptualHash::ComputeSignatureFromBGRA(expected.constBits(),128,96,expected.bytesPerLine());
        for(const auto& [ext, fmt] : std::vector<std::pair<const char*, const char*>>{
            {"bmp","bmp"}, {"dib","bmp"}, {"tif","tiff"}, {"tiff","tiff"},
            {"ppm","ppm"}, {"pgm","pgm"}, {"pbm","pbm"}, {"xbm","xbm"}, {"xpm","xpm"}, {"wbmp","wbmp"},
            {"ico","ico"}, {"icns","icns"}, {"jfif","jpeg"}, {"jpe","jpeg"}
        }) {
            const QString p=directory.filePath(QString("tier1.%1").arg(ext));
            QImage src = (QString(ext)=="ico" || QString(ext)=="icns") ? pattern.scaled(64,64,Qt::IgnoreAspectRatio,Qt::SmoothTransformation) : pattern;
            write(p, encode(src, fmt));
            require(VideoScanner::IsImageExtension(p.toStdWString()), "IsImageExtension Tier 1");
            auto f = info(p);
            require(StillImages::analyze(f, cancel) && f.frameLoaded && f.sigFirst.valid && f.thumbBGRA.size()==THUMB_W*THUMB_H*4, "Tier 1 analyze");
            if(QString(fmt)=="bmp" || QString(fmt)=="tiff" || QString(fmt)=="ppm" || QString(fmt)=="jpeg") {
                require(PerceptualHash::CompareSignatures(referenceSig, f.sigFirst) > 95.0, "Tier 1 visual match to pattern");
            }
        }
        // Misnamed extension (PNG bytes inside .bmp extension): content sniffing decodes accurately
        const QString misnamed = directory.filePath("png_inside.bmp");
        write(misnamed, png);
        auto misnamedInfo = info(misnamed);
        require(StillImages::analyze(misnamedInfo, cancel) && PerceptualHash::CompareSignatures(referenceSig, misnamedInfo.sigFirst) > 98.0, "content sniffing PNG inside .bmp");

        // Safe SVG rasterization + security rejection (XXE / external URL / script)
        const QByteArray validSvg = R"(<svg xmlns="http://www.w3.org/2000/svg" width="128" height="96" viewBox="0 0 128 96">
            <rect width="128" height="96" fill="#2060a0"/>
            <circle cx="48" cy="48" r="28" fill="#f0c030"/>
            <rect x="78" y="20" width="36" height="56" fill="#d03040"/>
        </svg>)";
        const QString svgPath = directory.filePath("vector.svg");
        write(svgPath, validSvg);
        auto svgInfo = info(svgPath);
        require(StillImages::analyze(svgInfo, cancel) && svgInfo.imageKind == ImageEvidenceKind::VectorRasterized && svgInfo.codec == L"SVG", "SVG rasterized evidence");

        for(const QByteArray& unsafeSvg : {
            QByteArray(R"(<!DOCTYPE svg [<!ENTITY xxe SYSTEM "file:///C:/Windows/win.ini">]><svg xmlns="http://www.w3.org/2000/svg"><text>&xxe;</text></svg>)"),
            QByteArray(R"(<svg xmlns="http://www.w3.org/2000/svg"><script>alert(1)</script><rect width="10" height="10"/></svg>)"),
            QByteArray(R"(<svg xmlns="http://www.w3.org/2000/svg"><image href="https://example.com/tracker.png" width="10" height="10"/></svg>)"),
            QByteArray(R"(<svg xmlns="http://www.w3.org/2000/svg"><image href="C:\Windows\win.ini" width="10" height="10"/></svg>)")
        }) {
            const QString badSvgPath = directory.filePath("unsafe.svg");
            write(badSvgPath, unsafeSvg);
            auto badSvg = info(badSvgPath);
            require(!StillImages::analyze(badSvg, cancel) && badSvg.error == HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Unsafe SVG rejected");
        }

        // <use> expansion: ten levels of ten references render 10^10 elements from about 4 KB; cycles never terminate.
        QByteArray laughs = R"(<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="10" height="10"><defs><rect id="l0" width="1" height="1"/>)";
        for(int level = 1; level <= 10; ++level) {
            laughs += QByteArray("<g id=\"l") + QByteArray::number(level) + "\">";
            for(int copy = 0; copy < 10; ++copy) laughs += QByteArray("<use xlink:href=\"#l") + QByteArray::number(level - 1) + "\"/>";
            laughs += "</g>";
        }
        laughs += R"(</defs><use xlink:href="#l10"/></svg>)";
        for(const QByteArray& expanding : {laughs,
            QByteArray(R"(<svg xmlns="http://www.w3.org/2000/svg" width="10" height="10"><g id="a"><use href="#b"/></g><g id="b"><use href="#a"/></g></svg>)")}) {
            const QString expandingPath = directory.filePath("expanding.svg");
            write(expandingPath, expanding);
            auto expandingInfo = info(expandingPath);
            require(!StillImages::analyze(expandingInfo, cancel) && expandingInfo.error == HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Expanding or cyclic SVG <use> rejected");
        }
        const QString reusedPath = directory.filePath("reused.svg");
        write(reusedPath, R"(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="32"><defs><circle id="dot" r="6" fill="#d03040"/></defs><rect width="64" height="32" fill="#2060a0"/><use href="#dot" x="10" y="16"/><use href="#dot" x="32" y="16"/><use href="#dot" x="54" y="16"/></svg>)");
        auto reusedInfo = info(reusedPath);
        require(StillImages::analyze(reusedInfo, cancel) && reusedInfo.imageKind == ImageEvidenceKind::VectorRasterized, "Ordinary SVG <use> reuse accepted");

        // --- NEW TIER 2: Animations (APNG, Animated GIF, Animated WebP) ---
        QImage frame2 = pattern.copy();
        { QPainter p(&frame2); p.fillRect(10, 10, 90, 70, Qt::yellow); p.fillRect(30, 25, 50, 40, Qt::magenta); }
        QImage frame3 = pattern.copy();
        { QPainter p(&frame3); p.fillRect(0, 0, 128, 96, Qt::cyan); p.fillRect(20, 15, 80, 60, Qt::darkRed); }
        QImage frame3Alt = pattern.copy();
        { QPainter p(&frame3Alt); p.fillRect(0, 0, 128, 96, Qt::black); p.fillRect(5, 5, 40, 80, Qt::green); }

        const QString apngA = directory.filePath("animA.apng");
        const QString apngA_copy = directory.filePath("animA_copy.png");
        const QString apngB_diffLater = directory.filePath("animB_diff_later.apng");
        write(apngA, buildSyntheticApng({pattern, frame2, frame3}));
        write(apngA_copy, buildSyntheticApng({pattern, frame2, frame3}));
        write(apngB_diffLater, buildSyntheticApng({pattern, frame2, frame3Alt}));

        auto animInfoA = info(apngA);
        auto animInfoA2 = info(apngA_copy);
        auto animInfoB = info(apngB_diffLater);
        auto staticInfo = info(directory.filePath("pattern.png"));
        require(StillImages::analyze(animInfoA, cancel) && animInfoA.imageKind == ImageEvidenceKind::AnimatedImage && animInfoA.imageFrameCount == 3 && animInfoA.samples.size() == 5, "APNG A analyze");
        require(StillImages::analyze(animInfoA2, cancel) && animInfoA2.imageKind == ImageEvidenceKind::AnimatedImage, "APNG A2 analyze");
        require(StillImages::analyze(animInfoB, cancel) && animInfoB.imageKind == ImageEvidenceKind::AnimatedImage, "APNG B analyze");
        require(StillImages::analyze(staticInfo, cancel) && staticInfo.imageKind == ImageEvidenceKind::StaticImage, "Static PNG analyze");

        bool animAccepted = false;
        double animScore = PerceptualHash::CompareImageFiles(animInfoA, animInfoA2, true, 90.0, &animAccepted);
        require(animAccepted && animScore > 98.0, "Identical animations match");
        animScore = PerceptualHash::CompareImageFiles(animInfoA, animInfoB, true, 90.0, &animAccepted);
        require(!animAccepted, "Animations with same first frame but different later frames MUST NOT match");
        animScore = PerceptualHash::CompareImageFiles(animInfoA, staticInfo, true, 90.0, &animAccepted);
        require(!animAccepted && animScore == 0.0, "Static image and animation sharing frame 1 MUST NOT match");

        // --- NEW TIER 3: Multi-Page TIFF & Multi-Size ICO ---
        const QString mpTiffA = directory.filePath("multipage_a.tif");
        const QString mpTiffA2 = directory.filePath("multipage_a2.tiff");
        const QString mpTiffB = directory.filePath("multipage_b_diff_p2.tif");
        const QString spTiff = directory.filePath("singlepage.tif");
        write(mpTiffA, buildMultiPageTiff({pattern, frame2}));
        write(mpTiffA2, buildMultiPageTiff({pattern, frame2}));
        write(mpTiffB, buildMultiPageTiff({pattern, frame3Alt}));
        write(spTiff, buildMultiPageTiff({pattern}));

        auto mpA = info(mpTiffA), mpA2 = info(mpTiffA2), mpB = info(mpTiffB), sp = info(spTiff);
        require(StillImages::analyze(mpA, cancel) && mpA.imageKind == ImageEvidenceKind::MultiPageDocument && mpA.imageFrameCount == 2 && mpA.samples.size() == 5, "Multi-page TIFF A analyze");
        require(StillImages::analyze(mpA2, cancel) && StillImages::analyze(mpB, cancel) && StillImages::analyze(sp, cancel), "Multi-page TIFF others analyze");
        require(sp.imageKind == ImageEvidenceKind::StaticImage && sp.imageFrameCount == 1, "Single-page TIFF kind");
        bool mpAccepted = false;
        double mpScore = PerceptualHash::CompareImageFiles(mpA, mpA2, true, 90.0, &mpAccepted);
        require(mpAccepted && mpScore > 98.0, "Identical multi-page TIFFs match");
        mpScore = PerceptualHash::CompareImageFiles(mpA, mpB, true, 90.0, &mpAccepted);
        require(!mpAccepted, "Multi-page TIFFs with different page 2 MUST NOT match");
        mpScore = PerceptualHash::CompareImageFiles(mpA, sp, true, 90.0, &mpAccepted);
        require(!mpAccepted && mpScore == 0.0, "Single-page vs Multi-page TIFF MUST NOT match");

        const QString msIcoPath = directory.filePath("multisize.ico");
        write(msIcoPath, buildMultiSizeIco(pattern.scaled(16,16), pattern.scaled(64,64)));
        auto msIco = info(msIcoPath);
        require(StillImages::analyze(msIco, cancel) && msIco.imageKind == ImageEvidenceKind::MultiSizeIcon && msIco.imageFrameCount == 2 && msIco.width == 64 && msIco.height == 64, "Multi-size ICO selects largest 64x64 entry");

        // --- NEW TIER 4: PSD, PFM/HDR/EXR, JXR (WIC), RAW (DNG Embedded Preview vs Developed), FFmpeg Image Formats ---
        const QString psdPath = directory.filePath("composite.psd");
        write(psdPath, buildSyntheticPsd(pattern));
        auto psdInfo = info(psdPath);
        require(StillImages::analyze(psdInfo, cancel) && psdInfo.imageKind == ImageEvidenceKind::CompositeDocument && PerceptualHash::CompareSignatures(referenceSig, psdInfo.sigFirst) > 95.0, "PSD composite decode & signature");

        const QString pfmPath = directory.filePath("float.pfm");
        write(pfmPath, buildSyntheticPfm(pattern));
        auto pfmInfo = info(pfmPath);
        require(StillImages::analyze(pfmInfo, cancel) && pfmInfo.imageKind == ImageEvidenceKind::TonemappedHdr && pfmInfo.sigFirst.valid, "PFM HDR tonemapped decode");

#ifdef _WIN32
        const QString jxrPath = directory.filePath("wic.jxr");
        if(writeWicJxr(jxrPath, pattern)) {
            auto jxrInfo = info(jxrPath);
            require(StillImages::analyze(jxrInfo, cancel) && PerceptualHash::CompareSignatures(referenceSig, jxrInfo.sigFirst) > 92.0, "WIC JXR decode & signature");
        }
#endif

        const QString dngPath = directory.filePath("camera.dng");
        write(dngPath, buildSyntheticDngWithPreview(jpeg));
        auto dngInfo = info(dngPath);
        require(StillImages::analyze(dngInfo, cancel) && dngInfo.imageKind == ImageEvidenceKind::RawEmbeddedPreview && PerceptualHash::CompareSignatures(referenceSig, dngInfo.sigFirst) > 95.0, "DNG RAW embedded preview decode");
        auto fakeDeveloped = dngInfo;
        fakeDeveloped.imageKind = ImageEvidenceKind::RawDeveloped;
        bool rawAccepted = false;
        double rawScore = PerceptualHash::CompareImageFiles(dngInfo, fakeDeveloped, true, 90.0, &rawAccepted);
        require(!rawAccepted && rawScore == 0.0, "RawEmbeddedPreview and RawDeveloped MUST NOT be equated");

        // Test FFmpeg-backed image codecs (GIF static & animated, TGA, PCX, QOI, SGI, Sun Raster, DPX, DDS, EXR, HDR, JP2, JLS) if ffmpeg is available
        QString ffmpegExe = QDir(QCoreApplication::applicationDirPath()).filePath("../.deps/ffmpeg-8.1/bin/ffmpeg.exe");
        if(!QFileInfo::exists(ffmpegExe)) ffmpegExe = QStandardPaths::findExecutable("ffmpeg");
        if(QFileInfo::exists(ffmpegExe)) {
            const QString srcPng = directory.filePath("pattern.png");
            for(const char* ext : {"gif", "tga", "pcx", "qoi", "sgi", "ras", "dpx", "dds", "exr", "hdr", "jp2", "jls"}) {
                const QString targetPath = directory.filePath(QString("ff.%1").arg(ext));
                QProcess proc;
                proc.start(ffmpegExe, {"-y", "-hide_banner", "-loglevel", "error", "-i", srcPng, targetPath});
                if(proc.waitForFinished(15000) && proc.exitCode() == 0 && QFileInfo::exists(targetPath)) {
                    auto ffInfo = info(targetPath);
                    require(StillImages::analyze(ffInfo, cancel) && ffInfo.frameLoaded && ffInfo.sigFirst.valid, (std::string("FFmpeg format analyze failed: ") + ext).c_str());
                }
            }
            // Generate a 3-frame animated GIF via ffmpeg and verify AnimatedImage evidence
            const QString f1Path = directory.filePath("gif_f1.png");
            const QString f2Path = directory.filePath("gif_f2.png");
            const QString f3Path = directory.filePath("gif_f3.png");
            write(f1Path, encode(pattern, "png"));
            write(f2Path, encode(frame2, "png"));
            write(f3Path, encode(frame3, "png"));
            const QString animGifPath = directory.filePath("anim3.gif");
            QProcess gifProc;
            gifProc.start(ffmpegExe, {"-y", "-hide_banner", "-loglevel", "error", "-framerate", "5", "-i", directory.filePath("gif_f%d.png"), animGifPath});
            if(gifProc.waitForFinished(15000) && gifProc.exitCode() == 0 && QFileInfo::exists(animGifPath)) {
                auto gifAnimInfo = info(animGifPath);
                require(StillImages::analyze(gifAnimInfo, cancel) && gifAnimInfo.imageKind == ImageEvidenceKind::AnimatedImage && gifAnimInfo.imageFrameCount == 3 && gifAnimInfo.samples.size() == 5, "Animated GIF analyze");
            }
        }

        // --- ScanCache v3 round-trip for AnimatedImage & MultiPageDocument ---
        {
            const QString cacheDb = directory.filePath("test-cache.sqlite");
            ScanCache cache(cacheDb);
            cache.store({animInfoA, mpA, psdInfo, dngInfo});
            VideoFileInfo restoredAnim = info(apngA);
            VideoFileInfo restoredMp = info(mpTiffA);
            require(cache.restore(restoredAnim) && restoredAnim.imageKind == ImageEvidenceKind::AnimatedImage && restoredAnim.imageFrameCount == 3 && restoredAnim.samples.size() == 5, "ScanCache v3 animation restore");
            require(cache.restore(restoredMp) && restoredMp.imageKind == ImageEvidenceKind::MultiPageDocument && restoredMp.imageFrameCount == 2 && restoredMp.samples.size() == 5, "ScanCache v3 multipage restore");
        }

        // --- Task 2: Scan Sources (Files + Folders, Per-Folder Subfolders, Excluded Extensions, Physical Self-Match Prevention) & Filename Criteria ---
        {
            const QString scanRootA = directory.filePath("scanA");
            const QString scanRootA_sub = directory.filePath("scanA/sub");
            const QString scanRootB = directory.filePath("scanB");
            const QString scanRootB_sub = directory.filePath("scanB/sub");
            const QString extraDir = directory.filePath("extraFiles");
            QDir().mkpath(scanRootA_sub);
            QDir().mkpath(scanRootB_sub);
            QDir().mkpath(extraDir);

            const QString fileATop = QDir(scanRootA).filePath("same_name.png");
            const QString fileASub = QDir(scanRootA_sub).filePath("hidden_in_nonrec.png");
            const QString fileBSub = QDir(scanRootB_sub).filePath("same_name.jpg");
            const QString fileExcludedGif = QDir(scanRootB).filePath("skip_me.gif");
            const QString fileDirect = QDir(extraDir).filePath("direct_added.png");

            write(fileATop, png);
            write(fileASub, png);
            write(fileBSub, encode(frame2, "jpeg")); // Different visual content, same stem "same_name"
            write(fileExcludedGif, png);
            write(fileDirect, png);

            ScanCriteria sc;
            sc.images = true;
            sc.includeSubfolders = true;
            sc.nonRecursiveFolders = { QDir::toNativeSeparators(scanRootA).toStdWString() }; // scanA is non-recursive; scanB is recursive
            sc.excludedExtensions = { L".gif" };
            // Add scanRootA, scanRootB, fileDirect, AND duplicate fileATop (physical deduplication test!)
            std::vector<std::wstring> roots = {
                QDir::toNativeSeparators(scanRootA).toStdWString(),
                QDir::toNativeSeparators(scanRootB).toStdWString(),
                QDir::toNativeSeparators(fileDirect).toStdWString(),
                QDir::toNativeSeparators(fileATop).toStdWString()
            };
            auto discovered = VideoScanner::DiscoverVideos(roots, true, cancel, nullptr, &sc);
            // Expected discovered files: fileATop, fileBSub, fileDirect (3 files total; fileASub skipped by nonRecursiveFolders; fileExcludedGif skipped by excludedExtensions; duplicate fileATop deduplicated)
            require(discovered.size() == 3, "DiscoverVideos: files+folders, per-folder nonRecursive, excludedExtensions, and physical deduplication");

            // Test matchFileName (same stem "same_name" across .png and .jpg)
            ScanCriteria nameCriteria = sc;
            nameCriteria.matchSize = false;
            nameCriteria.matchFirstFrame = false;
            nameCriteria.matchFileName = true;
            nameCriteria.matchExtension = false;
            std::vector<VideoGroup> nameGroups;
            VideoScanner::AnalyzeAndGroup(discovered, nameCriteria, nameGroups, cancel, {}, {}, nullptr, [&](auto& f) { StillImages::analyze(f, cancel); });
            require(nameGroups.size() == 1 && nameGroups[0].fileIndices.size() == 2, "matchFileName groups same_name.png and same_name.jpg");

            // With matchExtension = true, same_name.png and same_name.jpg do NOT match
            nameCriteria.matchExtension = true;
            nameGroups.clear();
            VideoScanner::AnalyzeAndGroup(discovered, nameCriteria, nameGroups, cancel, {}, {}, nullptr, [&](auto& f) { StillImages::analyze(f, cancel); });
            require(nameGroups.empty(), "matchFileName + matchExtension rejects different extensions");
        }

        puts("PASS still image backend: Tier 1-4 formats (PNG/JPEG/WebP/BMP/GIF/TIFF/TGA/ICO/ICNS/Netpbm/XBM/XPM/WBMP/SVG/APNG/Multi-page TIFF/Multi-size ICO/PSD/PFM/HDR/EXR/JXR/DDS/PCX/QOI/SGI/RAS/DPX/JP2/DNG RAW), EXIF orientations, white alpha matte, ICC to sRGB, safe SVG XXE/script/URL guards, animation temporal sampling & static/animation separation, multi-page count & page-2 separation, ScanCache v3, and scan source/criteria discovery; generated fixtures only");
        return 0;
    } catch(const std::exception& error) {
        fprintf(stderr,"FAIL image backend: %s\n",error.what());
        return 1;
    }
}
