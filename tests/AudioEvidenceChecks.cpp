#include "AudioEvidence.h"
#include "FileOperations.h"
#include <QCoreApplication>
#include <QDir>
#include <QProcess>
#include <QTemporaryDir>
#include <iostream>

int main(int argc,char** argv) {
    QCoreApplication app(argc,argv); if(argc!=2 && argc!=3) return 2;
    QTemporaryDir temp; if(!temp.isValid()) return 3;
    const QString ffmpeg=QString::fromLocal8Bit(argv[1]);
    auto run=[&](QStringList args) { QProcess p; p.start(ffmpeg,QStringList{"-v","error","-nostdin","-y"}+args); const bool ok=p.waitForFinished(60000) && p.exitCode()==0; if(!ok) std::cerr<<p.readAllStandardError().constData(); return ok; };
    auto path=[&](const char* name) { return temp.filePath(name); };
    const QString wave="0.18*sin(2*PI*(240*t+17*t*t))+0.09*sin(2*PI*(617*t+11*t*t))";
    if(!run({"-f","lavfi","-i","color=s=32x32:r=1:d=22","-f","lavfi","-i","aevalsrc="+wave+":s=48000:d=22","-c:v","mpeg4","-c:a","pcm_s16le",path("a.mkv")})) return 4;
    if(!run({"-i",path("a.mkv"),"-c:v","copy","-af","volume=0.3","-ar","44100","-c:a","aac","-b:a","128k",path("b.mkv")})) return 5;
    if(!run({"-i",path("a.mkv"),"-c:v","copy","-af","adelay=1000:all=1","-c:a","aac",path("offset.mkv")})) return 6;
    if(!run({"-i",path("a.mkv"),"-f","lavfi","-i","aevalsrc=0.2*sin(2*PI*(900*t+31*t*t)):s=48000:d=22","-map","0:v","-map","1:a","-c:v","copy","-c:a","aac",path("different.mkv")})) return 7;
    if(!run({"-i",path("a.mkv"),"-c:v","copy","-af","volume=0","-c:a","aac",path("silent.mkv")})) return 8;
    if(!run({"-i",path("a.mkv"),"-c:v","copy","-an",path("no.mkv")})) return 9;
    auto info=[&](const char* name) { VideoFileInfo f; f.fullPath=path(name).toStdWString(); f.identity=FileOperations::ReadIdentity(f.fullPath); f.duration100ns=220000000; return f; };
    std::atomic<bool> cancel=false; const auto a=info("a.mkv"); using S=AudioEvidence::Status;
    auto check=[&](const char* name,int64_t offset,S expected) { auto result=AudioEvidence::Compare(a,info(name),offset,cancel); std::cout<<name<<" status="<<int(result.status)<<" windows="<<result.windows<<" correlation="<<result.lowestCorrelation<<"\n"; return result.status==expected; };
    if(!check("a.mkv",0,S::SimilarSamples) || !check("b.mkv",0,S::SimilarSamples) || !check("offset.mkv",10000000,S::SimilarSamples) || !check("different.mkv",0,S::DifferentSamples) || !check("silent.mkv",0,S::Inconclusive) || !check("no.mkv",0,S::OneHasAudio)) return 10;
    auto no=info("no.mkv"); if(AudioEvidence::Compare(no,no,0,cancel).status!=S::NeitherHasAudio) return 11;
    auto changed=a; changed.identity.size++; if(AudioEvidence::Compare(changed,a,0,cancel).status!=S::Failed) return 12;
    if(AudioEvidence::Compare(a,a,200000000,cancel).status!=S::Inconclusive) return 13;
    cancel.store(true); if(AudioEvidence::Compare(a,a,0,cancel).status!=S::Cancelled) return 14;
    std::cout<<"PASS local generated audio evidence: re-encode, rate, gain, offset, changed sound, silence, absent track, identity, short clip and cancellation\n";
    if(argc==3) {
        QProcess gui; gui.setProcessChannelMode(QProcess::ForwardedChannels);
        gui.start(QString::fromLocal8Bit(argv[2]),{"--audio-evidence-ui-check","--",path("a.mkv"),path("b.mkv")});
        if(!gui.waitForFinished(60000) || gui.exitCode()!=0) return 15;
    }
    return 0;
}
