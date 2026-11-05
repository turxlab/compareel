#include "SubtitleInfo.h"
#include <QCoreApplication>
#include <QDir>
#include <stdexcept>

int main(int argc,char** argv) {
    QCoreApplication app(argc,argv); const auto args=app.arguments();
    try {
        if(args.size()!=3) throw std::runtime_error("usage: SubtitleChecks subtitled.mp4 plain.mp4");
        std::atomic<bool> cancel{false};
        auto path=QDir::toNativeSeparators(QFileInfo(args[1]).absoluteFilePath()); auto identity=FileOperations::ReadIdentity(path.toStdWString());
        auto found=inspectSubtitles(path,identity,cancel);
        if(found.status!=SubtitleInfo::Complete || found.tracks.size()!=1 || !found.tracks[0].contains("mov_text")) throw std::runtime_error("embedded track not detected");
        auto plain=QDir::toNativeSeparators(QFileInfo(args[2]).absoluteFilePath()); auto plainIdentity=FileOperations::ReadIdentity(plain.toStdWString());
        auto empty=inspectSubtitles(plain,plainIdentity,cancel);
        if(empty.status!=SubtitleInfo::Complete || !empty.tracks.empty()) throw std::runtime_error("plain video misreported");
        auto changed=identity; ++changed.index;
        if(inspectSubtitles(path,changed,cancel).status!=SubtitleInfo::Changed) throw std::runtime_error("changed media accepted");
        cancel=true; if(inspectSubtitles(path,identity,cancel).status!=SubtitleInfo::Cancelled) throw std::runtime_error("pre-cancel ignored");
        if(FileOperations::ReadIdentity(path.toStdWString())!=identity || FileOperations::ReadIdentity(plain.toStdWString())!=plainIdentity) throw std::runtime_error("source modified");
        puts("PASS subtitles: bundled FFmpeg embedded/plain videos, cancelled/changed cases, sources unchanged");
    } catch(const std::exception& error) { fprintf(stderr,"FAIL %s\n",error.what()); return 1; }
    return 0;
}
