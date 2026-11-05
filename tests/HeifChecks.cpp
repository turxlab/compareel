#include "ImageReader.h"
#include "FileOperations.h"
#include "PerceptualHash.h"
#include <libheif/heif.h>
#include <libheif/heif_properties.h>
#include <QCoreApplication>
#include <QColorSpace>
#include <QFile>
#include <QImageReader>
#include <QProcess>
#include <QTemporaryDir>
#include <QTransform>
#include <stdexcept>
static void require(bool ok,const char* why) { if(!ok) throw std::runtime_error(why); }
static VideoFileInfo info(const QString& path) { VideoFileInfo f; f.fullPath=path.toStdWString(); f.identity=FileOperations::ReadIdentity(f.fullPath); f.fileSize=f.identity.size; return f; }
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv); QImageReader::setAllocationLimit(256);
    try {
        require(argc==3,"usage HeifChecks libheif-test-data ffmpeg.exe");
        QTemporaryDir root; require(root.isValid(),"private fixtures"); std::atomic<bool> cancel{false}; HRESULT error;
        const auto source=QDir(app.arguments()[1]).filePath("rainbow-451x461.heic");
        auto heic=info(source); const auto original=StillImages::read(heic,cancel,error);
        require(!original.isNull() && SUCCEEDED(error) && original.size()==QSize(451,461),"upstream HEIC reference decode/dimensions");
        const auto unicode=root.filePath(QString::fromUtf8("Örnek 資料.heif")); require(QFile::copy(source,unicode),"Unicode HEIF copy");
        auto file=info(unicode); require(StillImages::analyze(file,cancel) && file.codec==L"HEIC" && file.sigFirst.valid,"HEIF extension/analyze/hash");
        {
            const auto path=QDir(app.arguments()[1]).filePath("clap_cropped_irot_imir.avif");
            QFile input(path); require(input.open(QIODevice::ReadOnly),"geometry fixture"); const auto bytes=input.readAll();
            auto context=heif_context_alloc(); require(context,"geometry context");
            require(heif_context_read_from_memory_without_copy(context,bytes.constData(),bytes.size(),nullptr).code==heif_error_Ok,"geometry parse");
            heif_item_id id=0; heif_image_handle* handle=nullptr; heif_image* decoded=nullptr;
            require(heif_context_get_primary_image_ID(context,&id).code==heif_error_Ok && heif_context_get_primary_image_handle(context,&handle).code==heif_error_Ok,"geometry handle");
            auto options=heif_decoding_options_alloc(); options->ignore_transformations=1;
            require(heif_decode_image(handle,&decoded,heif_colorspace_RGB,heif_chroma_interleaved_RGBA,options).code==heif_error_Ok,"untransformed geometry pixels");
            int stride=0; auto pixels=heif_image_get_plane_readonly(decoded,heif_channel_interleaved,&stride);
            QImage expected=QImage(pixels,heif_image_get_width(decoded,heif_channel_interleaved),heif_image_get_height(decoded,heif_channel_interleaved),stride,QImage::Format_RGBA8888).copy();
            heif_property_id properties[16]; const int count=heif_item_get_transformation_properties(context,id,properties,16); require(count>=3 && count<=16,"crop/rotation/mirror fixture properties");
            for(int i=0;i<count;++i) {
                const auto type=heif_item_get_property_type(context,id,properties[i]);
                if(type==heif_item_property_type_transform_crop) { int left=0,top=0,right=0,bottom=0; heif_item_get_property_transform_crop_borders(context,id,properties[i],expected.width(),expected.height(),&left,&top,&right,&bottom); expected=expected.copy(left,top,expected.width()-left-right,expected.height()-top-bottom); }
                if(type==heif_item_property_type_transform_rotation) expected=expected.transformed(QTransform().rotate(-heif_item_get_property_transform_rotation_ccw(context,id,properties[i])));
                if(type==heif_item_property_type_transform_mirror) { const auto mirror=heif_item_get_property_transform_mirror(context,id,properties[i]); expected=expected.mirrored(mirror==heif_transform_mirror_direction_horizontal,mirror==heif_transform_mirror_direction_vertical); }
            }
            heif_image_release(decoded); heif_image_handle_release(handle); heif_decoding_options_free(options); heif_context_free(context);
            file=info(path); const auto actual=StillImages::read(file,cancel,error);
            expected=expected.convertedTo(QImage::Format_RGB32);
            expected.setColorSpace(actual.colorSpace());
            if(!actual.isNull()) {
                const auto a=PerceptualHash::ComputeSignatureFromBGRA(actual.constBits(),actual.width(),actual.height(),actual.bytesPerLine());
                const auto b=PerceptualHash::ComputeSignatureFromBGRA(expected.constBits(),expected.width(),expected.height(),expected.bytesPerLine());
                fprintf(stderr,"geometry actual=%dx%d expected=%dx%d similarity=%.3f\n",actual.width(),actual.height(),expected.width(),expected.height(),PerceptualHash::CompareSignatures(a,b));
            }
            require(SUCCEEDED(error) && actual==expected,"container geometry must match independent pixel crop/rotate/mirror");
        }
        auto alpha=info(QDir(app.arguments()[1]).filePath("with-alpha-512x512.heic"));
        const auto alphaImage=StillImages::read(alpha,cancel,error);
        require(SUCCEEDED(error) && alphaImage.size()==QSize(512,512) && alphaImage.format()==QImage::Format_RGB32,"HEIC alpha normalization");
        QImage pattern(128,96,QImage::Format_RGB32);
        for(int y=0;y<96;++y) for(int x=0;x<128;++x) pattern.setPixel(x,y,qRgb(x*2,y*2,(x+y)%256));
        const auto png=root.filePath("pattern.png"); require(pattern.save(png),"generated PNG");
        auto encode=[&](const QString& path,const QStringList& extra) {
            QProcess p; QStringList args{"-hide_banner","-loglevel","error","-nostdin","-y","-i",png,"-frames:v","1","-c:v","libaom-av1","-cpu-used","8","-crf","0","-b:v","0","-pix_fmt","yuv444p","-color_primaries","bt709","-color_trc","iec61966-2-1","-colorspace","bt470bg","-color_range","pc"};
            args+=extra; args+=path; p.start(app.arguments()[2],args); if(!p.waitForFinished(30000)) { p.kill(); p.waitForFinished(); return false; }
            if(p.exitCode()) fprintf(stderr,"%s",p.readAllStandardError().constData()); return p.exitCode()==0;
        };
        for(bool tenBit:{false,true}) {
            const auto path=root.filePath(tenBit?"sdr10.avif":"sdr8.avif"); require(encode(path,tenBit?QStringList{"-pix_fmt","yuv444p10le"}:QStringList{}),"generated AVIF");
            file=info(path); require(StillImages::analyze(file,cancel) && file.codec==L"AVIF","8/10-bit SDR AVIF analyze");
            const auto reference=PerceptualHash::ComputeSignatureFromBGRA(pattern.constBits(),128,96,pattern.bytesPerLine());
            require(PerceptualHash::CompareSignatures(reference,file.sigFirst)>97,"AVIF colors/structure");
        }
        const auto hdr=root.filePath("hdr.avif"); require(encode(hdr,{"-vf","setparams=color_trc=smpte2084:color_primaries=bt709:colorspace=bt470bg","-color_trc","smpte2084"}),"HDR metadata fixture");
        { QProcess probe; probe.start(QFileInfo(app.arguments()[2]).dir().filePath(QFileInfo(app.arguments()[2]).suffix()=="exe"?"ffprobe.exe":"ffprobe"),{"-v","error","-show_entries","stream=color_transfer,color_primaries,color_space","-of","json",hdr}); require(probe.waitForFinished(10000) && probe.exitCode()==0,"HDR fixture metadata"); fprintf(stderr,"%s",probe.readAllStandardOutput().constData()); }
        file=info(hdr); const auto hdrImage=StillImages::read(file,cancel,error);
        fprintf(stderr,"HDR null=%d hr=%08lx\n",hdrImage.isNull(),static_cast<unsigned long>(error));
        // PQ/HLG stills are tone mapped to SDR sRGB (visual evidence only) instead of being rejected.
        require(!hdrImage.isNull() && SUCCEEDED(error) && hdrImage.size()==QSize(128,96) && hdrImage.format()==QImage::Format_RGB32,"HDR tone mapped to SDR");
        file=heic; cancel=true; require(StillImages::read(file,cancel,error).isNull() && error==HRESULT_FROM_WIN32(ERROR_CANCELLED),"HEIC cancellation"); cancel=false;
        const auto broken=root.filePath("broken.heic"); { QFile f(broken); require(f.open(QIODevice::WriteOnly) && f.write("invalid")==7,"bad header fixture"); }
        file=info(broken); require(StillImages::read(file,cancel,error).isNull() && FAILED(error),"invalid HEIC rejected");
        const auto sequence=root.filePath("animated.avif");
        { QProcess p; p.start(app.arguments()[2],{"-hide_banner","-loglevel","error","-nostdin","-y","-loop","1","-framerate","1","-i",png,"-frames:v","2","-c:v","libaom-av1","-cpu-used","8",sequence}); require(p.waitForFinished(30000) && p.exitCode()==0,"animated AVIF fixture"); }
        file=info(sequence); require(StillImages::read(file,cancel,error).isNull() && FAILED(error),"animation must not silently compare its first frame");
        require(FileOperations::ReadIdentity(heic.fullPath)==heic.identity,"source changed");
        puts("PASS HEIC/HEIF/AVIF: upstream public rainbow+alpha, independent crop/rotation/mirror pixels, Unicode, actual analyze, generated SDR8/10 AVIF, color/structure, HDR tone mapping, cancel/bad-header refusal; no user media");
    } catch(const std::exception& e) { fprintf(stderr,"FAIL HEIF: %s\n",e.what()); return 1; }
}
