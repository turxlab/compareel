#include "ImageReader.h"
#include "platform/Platform.h"
#include "VideoScanner.h"
#include <QFileInfo>
#include "FileOperations.h"
#include "PerceptualHash.h"
#include "ImageCandidates.h"
#include <QGuiApplication>
#include <QFile>
#include <QDir>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <iostream>
#include <QtEndian>
#if __has_include(<QtZlib/zlib.h>)
#include <QtZlib/zlib.h>
#else
#include <zlib.h>
#endif
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
int main(int argc,char** argv) {
 QGuiApplication app(argc,argv); if(argc!=3) return 2; bool good=true; Platform::ComInit com; Platform::MediaRuntime media;
 std::atomic<bool> cancel=false;
 FrameSignature sig; sig.valid=true; sig.dHashH=0x123456789abcdef0; sig.dHashV=0x3344556677889900; sig.aHash=0x0123456789abcdef;
 VideoFileInfo a,b; a.frameLoaded=b.frameLoaded=true; a.imageKind=b.imageKind=ImageEvidenceKind::AnimatedImage; a.duration100ns=b.duration100ns=100000000; a.imageFrameCount=b.imageFrameCount=10;
 a.sigFirst=b.sigFirst=sig; b.sigFirst.dHashH=~sig.dHashH; b.sigFirst.dHashV=~sig.dHashV; b.sigFirst.aHash=~sig.aHash;
 a.samples=b.samples=std::vector<FrameSignature>(5,sig);
 ScanCriteria sc; sc.images=true; sc.matchFirstFrame=true; sc.matchDuration=false; sc.matchSize=false; sc.matchTransforms=false; sc.minFrameSimilarity=90;
 std::vector<VideoFileInfo> files{a,b}; std::vector<int> ids{0,1}; ImageCandidates index(files,ids,false,cancel); auto hits=index.query(a,90);
 std::cout<<"ANIMATION_INDEX direct="<<VideoScanner::FilesMatch(a,b,sc)<<" indexed_has_pair="<<(std::find(hits.begin(),hits.end(),1)!=hits.end())<<"\n";
 std::vector<VideoGroup> twoGroups,manyGroups; VideoScanner::GroupKnownFiles(files,ids,sc,twoGroups,cancel);
 for(int i=2;i<64;++i) { VideoFileInfo f; f.frameLoaded=true; f.sigFirst=sig; f.sigFirst.dHashH^=uint64_t(i)*0x9e3779b97f4a7c15ULL; f.samples.clear(); files.push_back(f); ids.push_back(i); }
 VideoScanner::GroupKnownFiles(files,ids,sc,manyGroups,cancel);
 bool pairPresent=false; for(const auto& group:manyGroups) if(std::find(group.fileIndices.begin(),group.fileIndices.end(),0)!=group.fileIndices.end() && std::find(group.fileIndices.begin(),group.fileIndices.end(),1)!=group.fileIndices.end()) pairPresent=true;
 std::cout<<"REAL_GROUPING groups_2="<<twoGroups.size()<<" groups_64="<<manyGroups.size()<<" animation_pair_64="<<pairPresent<<"\n";
 good=twoGroups.size()==1 && pairPresent;
 QTemporaryDir dir; QImage red(16,16,QImage::Format_RGB32); red.fill(Qt::red); auto png=dir.filePath("private-fixture.png"); red.save(png);
 auto imageFile=[](const QString& path) { VideoFileInfo f; f.fullPath=path.toStdWString(); f.identity=FileOperations::ReadIdentity(f.fullPath); f.fileSize=f.identity.size; return f; };
 for(const auto size:{QSize(9000,8000),QSize(800,17000)}) {
  const auto path=dir.filePath(QString("large-%1.jpg").arg(size.width()));
  { QImage source(size,QImage::Format_Grayscale8); source.fill(160); if(!source.save(path,"JPEG")) return 3; }
  auto source=imageFile(path); HRESULT err; StillImages::ImageExtendedInfo info;
  const auto preview=StillImages::read(source,cancel,err,nullptr,true,&info);
  good=good && !preview.isNull() && preview.width()<=4096 && preview.height()<=4096 && info.sourceSize==size;
  good=good && StillImages::analyze(source,cancel) && source.width==uint32_t(size.width()) && source.height==uint32_t(size.height()) && source.sigFirst.valid;
  if(size.width()==9000) {
   QFile jpeg(path); jpeg.open(QIODevice::ReadOnly); auto bytes=jpeg.readAll(); jpeg.close();
   auto exif=QByteArray("Exif\0\0",6)+QByteArray::fromHex("49492a0008000000010012010300010000000600000000000000");
   QByteArray marker=QByteArray::fromHex("ffe1"); QByteArray length(2,Qt::Uninitialized); qToBigEndian<quint16>(quint16(exif.size()+2),length.data()); marker+=length+exif;
   bytes.insert(2,marker); jpeg.open(QIODevice::WriteOnly|QIODevice::Truncate); jpeg.write(bytes); jpeg.close();
   source=imageFile(path); const auto rotated=StillImages::read(source,cancel,err,nullptr,true,&info);
   good=good && !rotated.isNull() && info.sourceSize==size.transposed() && rotated.height()>rotated.width();
   good=good && StillImages::analyze(source,cancel) && source.width==uint32_t(size.height()) && source.height==uint32_t(size.width());
  }
 }
 QFile pngInput(png); pngInput.open(QIODevice::ReadOnly); const auto pngBytes=pngInput.readAll(); pngInput.close();
 auto pngChunk=[](QByteArray type,QByteArray payload) { QByteArray result(4,Qt::Uninitialized); qToBigEndian<quint32>(quint32(payload.size()),result.data()); result+=type+payload; const auto checksum=crc32(0,reinterpret_cast<const Bytef*>(result.constData()+4),uInt(result.size()-4)); QByteArray crc(4,Qt::Uninitialized); qToBigEndian<quint32>(quint32(checksum),crc.data()); return result+crc; };
 for(int kind=0;kind<3;++kind) {
  auto bytes=pngBytes;
  if(kind==0) bytes+="trailing application metadata";
  else {
   auto exif=QByteArray::fromHex("49492a0008000000010012010300010000000000000000000000");
   qToLittleEndian<quint16>(kind==1?0:9,exif.data()+18);
   bytes.insert(33,pngChunk("eXIf",exif));
  }
  const auto path=dir.filePath(QString("png-metadata-%1.png").arg(kind)); QFile output(path); output.open(QIODevice::WriteOnly); output.write(bytes); output.close();
  auto source=imageFile(path); HRESULT err; const auto result=StillImages::read(source,cancel,err);
  good=good && (kind==2?result.isNull() && FAILED(err):!result.isNull() && SUCCEEDED(err));
 }
 std::cout<<"LARGE_JPEG_AND_PNG_METADATA "<<(good?"PASS":"FAIL")<<"\n";
 auto read=[&](QString path) { VideoFileInfo f; f.fullPath=path.toStdWString(); f.identity=FileOperations::ReadIdentity(f.fullPath); f.fileSize=f.identity.size; StillImages::ImageExtendedInfo ex; HRESULT hr; auto im=StillImages::read(f,cancel,hr,nullptr,true,&ex); std::cout<<"DECODE file="<<QFileInfo(path).fileName().toStdString()<<" valid="<<!im.isNull()<<" hr="<<std::hex<<uint32_t(hr)<<std::dec<<" count="<<ex.frameOrPageCount<<" duration="<<ex.totalDuration100ns; if(!im.isNull()) std::cout<<" red_center="<<(im.pixelColor(im.width()/2,im.height()/2)==QColor(Qt::red)); std::cout<<"\n"; };
 for(int n=0;n<3;++n) {
  auto path=dir.filePath(n?"external-space.svg":"external.svg"); QFile f(path); f.open(QIODevice::WriteOnly);
  f.write((QString("<svg xmlns='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink' width='16' height='16'><image width='16' height='16' xlink:href%1='%2'/></svg>").arg(n?" ":"").arg(n==2?png:"file:///"+png)).toUtf8()); f.close(); read(path);
 }
 auto gif=dir.filePath("over600.gif"); QProcess ff; ff.start(QString::fromLocal8Bit(argv[1]),{"-v","error","-y","-f","lavfi","-i","testsrc2=s=16x16:r=20:d=31","-loop","0",gif}); ff.waitForFinished(30000); if(ff.exitCode()!=0) return 3;
 VideoFileInfo f; f.fullPath=gif.toStdWString(); f.identity=FileOperations::ReadIdentity(f.fullPath); HRESULT hr=S_OK;
 good=good && StillImages::read(f,cancel,hr,nullptr,true).isNull() && hr==HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
 for(const auto& name:{"external.svg","external-space.svg"}) { auto path=dir.filePath(name); f.fullPath=path.toStdWString(); f.identity=FileOperations::ReadIdentity(f.fullPath); hr=S_OK; good=good && StillImages::read(f,cancel,hr).isNull() && FAILED(hr); }
 auto safe=dir.filePath("safe.svgz"); QByteArray xml="<svg xmlns='http://www.w3.org/2000/svg' width='16' height='16'><rect width='16' height='16' fill='red'/></svg>";
 auto compressed=[](QByteArray bytes) { z_stream s{}; deflateInit2(&s,6,Z_DEFLATED,31,8,Z_DEFAULT_STRATEGY); QByteArray out(bytes.size()+128,Qt::Uninitialized); s.next_in=reinterpret_cast<Bytef*>(bytes.data()); s.avail_in=uInt(bytes.size()); s.next_out=reinterpret_cast<Bytef*>(out.data()); s.avail_out=uInt(out.size()); deflate(&s,Z_FINISH); out.resize(qsizetype(s.total_out)); deflateEnd(&s); return out; };
 QFile gz(safe); gz.open(QIODevice::WriteOnly); gz.write(compressed(xml)); gz.close(); f.fullPath=safe.toStdWString(); f.identity=FileOperations::ReadIdentity(f.fullPath); good=good && !StillImages::read(f,cancel,hr).isNull();
 QFile evil(dir.filePath("external-space.svg")); evil.open(QIODevice::ReadOnly); xml=evil.readAll(); evil.close(); gz.open(QIODevice::WriteOnly|QIODevice::Truncate); gz.write(compressed(xml)); gz.close(); f.identity=FileOperations::ReadIdentity(f.fullPath); good=good && StillImages::read(f,cancel,hr).isNull();
 const auto shortGif=dir.filePath("three.gif"); ff.start(QString::fromLocal8Bit(argv[1]),{"-v","error","-y","-f","lavfi","-i","testsrc2=s=48x32:r=10:d=0.3","-loop","0",shortGif}); if(!ff.waitForFinished(30000) || ff.exitCode()!=0) return 3;
 QImage page1(32,32,QImage::Format_RGB32),page2(32,32,QImage::Format_RGB32); page1.fill(Qt::red); page2.fill(Qt::blue);
 const auto tiff=dir.filePath("pages.tif"); QFile tf(tiff); tf.open(QIODevice::WriteOnly); tf.write(buildMultiPageTiff({page1,page2})); tf.close();
 QProcess ui; ui.setProcessChannelMode(QProcess::ForwardedChannels);
 if(QString::fromLocal8Bit(argv[2]).contains("release")) { auto env=QProcessEnvironment::systemEnvironment(); const auto windows=env.value("SystemRoot"); env.insert("PATH",windows+"/System32;"+windows); env.remove("QT_PLUGIN_PATH"); ui.setProcessEnvironment(env); }
 ui.start(QString::fromLocal8Bit(argv[2]),{"--image-guard-check","--",shortGif,tiff});
 good=good && ui.waitForFinished(30000) && ui.exitCode()==0;
 std::cout<<(good?"PASS":"FAIL")<<" Image guard SVG XML/gzip external file guards, 620-frame rejection, temporal indexed grouping and real UI\n";
 return good?0:4;
}

