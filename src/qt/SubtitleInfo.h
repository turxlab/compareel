#pragma once
#include "FFmpegReader.h"
#include "FileOperations.h"
#include <QString>
#include <QStringList>

struct SubtitleInfo {
    enum Status { Complete,Failed,Cancelled,Changed } status=Failed;
    QStringList tracks;
};

// Uses the bundled FFmpeg libraries, so it works without an external FFprobe.
inline SubtitleInfo inspectSubtitles(const QString& path,const FileIdentity& identity,std::atomic<bool>& cancel) {
    if(cancel.load()) return {SubtitleInfo::Cancelled,{}};
    if(!identity.valid || FileOperations::ReadIdentity(path.toStdWString())!=identity) return {SubtitleInfo::Changed,{}};
    std::vector<std::string> tracks;
    const bool read=ReadFFmpegSubtitleTracks(path.toStdWString(),tracks,&cancel);
    if(cancel.load()) return {SubtitleInfo::Cancelled,{}};
    if(!read) return {};
    if(FileOperations::ReadIdentity(path.toStdWString())!=identity) return {SubtitleInfo::Changed,{}};
    SubtitleInfo result{SubtitleInfo::Complete,{}};
    for(const auto& track:tracks) result.tracks<<QString::fromUtf8(track.c_str());
    return result;
}
