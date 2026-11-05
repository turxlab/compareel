#include "PartialClipSearch.h"
#include "platform/Platform.h"
#include <QCoreApplication>
#include <QProcess>
#include <QTemporaryDir>
#include <iostream>
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv); if(argc<2) return 2;
    QTemporaryDir temp; if(!temp.isValid()) return 3;
    auto path=[&](const char* name) { return temp.filePath(name); };
    auto run=[&](QStringList args) { QProcess p; p.start(QString::fromLocal8Bit(argv[1]),QStringList{"-v","error","-nostdin","-y"}+args); const bool ok=p.waitForFinished(60000) && p.exitCode()==0; if(!ok) std::cerr<<p.readAllStandardError().constData(); return ok; };
    // The Windows test FFmpeg is an LGPL build with OpenH264; other full builds have x264 instead.
    const QString h264=[&] { QProcess p; p.start(QString::fromLocal8Bit(argv[1]),{"-hide_banner","-encoders"}); p.waitForFinished(30000); return p.readAllStandardOutput().contains(" libopenh264 ")?"libopenh264":"libx264"; }();
    if(!run({"-f","lavfi","-i","testsrc2=s=160x96:r=12:d=20","-c:v",h264,"-b:v","1M",path("full.mp4")})) return 4;
    if(!run({"-ss","6","-i",path("full.mp4"),"-t","10","-c:v",h264,"-b:v","600k",path("trim.mp4")})) return 5;
    if(!run({"-f","lavfi","-i","smptebars=s=160x96:r=12:d=12","-c:v",h264,path("static.mp4")})) return 6;
    if(!run({"-i",path("static.mp4"),"-c","copy",path("static-copy.mp4")})) return 7;
    if(!run({"-f","lavfi","-i","mandelbrot=s=160x96:r=12","-t","12","-c:v",h264,"-b:v","600k",path("other.mp4")})) return 8;
    Platform::ComInit com; Platform::MediaRuntime media;
    ScanCriteria criteria; criteria.minFileSize=1; criteria.crossFolderOnly=false;
    std::atomic<bool> cancel=false; unsigned progress=0;
    auto result=PartialClipSearch::Find({temp.path().toStdWString()},criteria,cancel,[&](const ScanProgress&) { ++progress; });
    bool found=false,bad=false;
    for(const auto& match:result.matches) {
        const auto& a=result.files[match.left]; const auto& b=result.files[match.right];
        std::wcout<<a.fileName<<L" / "<<b.fileName<<L" offset="<<double(match.offset)/1e7<<L" overlap="<<double(match.duration)/1e7<<L" score="<<match.similarity<<L"\n";
        if((a.fileName==L"full.mp4" && b.fileName==L"trim.mp4") || (a.fileName==L"trim.mp4" && b.fileName==L"full.mp4")) found=std::abs(std::abs(match.offset)-60000000)<5000000 && match.duration>=60000000;
        else bad=true;
    }
    bool good=found && !bad && !result.cancelled && !result.failed && result.files.size()==5 && progress>0;
    cancel.store(true); auto stopped=PartialClipSearch::Find({temp.path().toStdWString()},criteria,cancel); good=good && stopped.cancelled && stopped.matches.empty();
    if(argc==3 && good) {
        QProcess gui; gui.setProcessChannelMode(QProcess::ForwardedChannels); gui.start(QString::fromLocal8Bit(argv[2]),{"--partial-search-ui-check","--",temp.path()});
        good=gui.waitForFinished(60000) && gui.exitCode()==0;
    }
    std::cout<<(good?"PASS":"FAIL")<<" folder-wide indexed partial search, trim offset, static/unrelated rejection and cancellation; synthetic local media\n";
    return good?0:9;
}
