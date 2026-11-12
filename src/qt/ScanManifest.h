#pragma once
#include "VideoScanner.h"
#include "PathKey.h"
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>

namespace ScanManifest {
inline QByteArray context(const ScanCriteria& c) {
    QByteArray bytes; QDataStream out(&bytes,QIODevice::WriteOnly); out.setVersion(QDataStream::Qt_6_8);
    out<<c.matchSize<<c.matchDuration<<c.matchResolution<<c.matchFirstFrame<<c.matchContent<<c.matchVisualSamples
       <<c.matchTransforms<<c.matchFileName<<c.matchExtension<<c.crossFolderOnly<<c.includeSubfolders<<quint64(c.minFileSize)<<quint64(c.maxFileSize)
       <<quint64(0)<<0.0<<qint64(c.durationTol100ns)<<c.minFrameSimilarity; // two zeros: retired size tolerance, kept so paused scans still resume
    for(const auto* list:{&c.comparisonRoots,&c.excludedFolders,&c.protectedFolders,&c.extensions,&c.excludedExtensions,&c.nonRecursiveFolders}) {
        out<<quint64(list->size()); for(const auto& value:*list) out<<QString::fromStdWString(value);
    }
    if(c.images) out<<QStringLiteral("still-images-v1");
    // Sorting and worker limits do not change the frozen input list or matching criteria.
    return QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex();
}
enum class SaveStatus { Saved, InvalidInput, TooLarge, WriteFailed };
inline bool save(const QString& path,const ScanCriteria& criteria,const std::vector<VideoFileInfo>& files,SaveStatus* outStatus=nullptr) {
    auto finish=[&](SaveStatus s) { if(outStatus) *outStatus=s; return s==SaveStatus::Saved; };
    if(files.size()>100000) return finish(SaveStatus::TooLarge);
    QJsonArray rows;
    for(const auto& file:files) {
        if(!file.identity.valid || file.deleted) return finish(SaveStatus::InvalidInput);
        rows.append(QJsonArray{QString::fromStdWString(file.fullPath),QString::number(file.identity.volume),
            QString::number(file.identity.index),QString::number(file.identity.size),QString::number(file.identity.modified),file.protectedFile,file.protectedByFolder});
    }
    const auto bytes=QJsonDocument(QJsonObject{{"version",criteria.images?2:1},{"context",QString::fromLatin1(context(criteria))},{"files",rows}}).toJson(QJsonDocument::Compact);
    if(bytes.size()>32*1024*1024) return finish(SaveStatus::TooLarge);
    QSaveFile output(path);
    if(output.open(QIODevice::WriteOnly) && output.write(bytes)==bytes.size() && output.commit()) return finish(SaveStatus::Saved);
    return finish(SaveStatus::WriteFailed);
}
inline bool load(const QString& manifestPath,const ScanCriteria& criteria,std::vector<VideoFileInfo>& files) {
    QFile input(manifestPath); if(!input.open(QIODevice::ReadOnly) || input.size()>32*1024*1024) return false;
    const auto bytes=input.read(32*1024*1024+1);
    if(bytes.size()>32*1024*1024 || input.error()!=QFile::NoError) return false;
    const auto doc=QJsonDocument::fromJson(bytes); const auto object=doc.object();
    if(!doc.isObject() || object["version"].toInt()!=(criteria.images?2:1) || object["context"].toString().toLatin1()!=context(criteria) || !object["files"].isArray()) return false;
    const auto rows=object["files"].toArray(); if(rows.size()>100000) return false;
    std::vector<VideoFileInfo> restored; QSet<QString> paths,physical;
    for(const auto& item:rows) {
        if(!item.isArray()) return false; const auto row=item.toArray();
        if(row.size()!=7 || !row[5].isBool() || !row[6].isBool()) return false;
        const auto path=row[0].toString();
        if(path.isEmpty() || path.size()>32767 || path.contains(QChar(0)) || !QFileInfo(path).isAbsolute()) return false;
        const auto normalized=QDir::toNativeSeparators(QDir::cleanPath(path));
        if(paths.contains(pathKey(normalized))) return false; paths.insert(pathKey(normalized));
        VideoFileInfo file; bool ok=false;
        uint64_t* fields[]{&file.identity.volume,&file.identity.index,&file.identity.size,&file.identity.modified};
        for(int i=0;i<4;++i) { *fields[i]=row[i+1].toString().toULongLong(&ok); if(!ok) return false; }
        file.identity.valid=true;
        if(HasUniqueFileIndex(file.identity)) {
            const auto id=QString::number(file.identity.volume)+":"+QString::number(file.identity.index);
            if(physical.contains(id)) return false;
            physical.insert(id);
        }
        file.fullPath=normalized.toStdWString(); file.fileName=QFileInfo(path).fileName().toStdWString();
        file.folderPath=QFileInfo(path).absolutePath().toStdWString(); file.fileSize=file.identity.size;
        file.protectedFile=row[5].toBool(); file.protectedByFolder=row[6].toBool();
        if(!(criteria.images?VideoScanner::IsImageExtension(file.fullPath):VideoScanner::IsVideoExtension(file.fullPath)) || std::none_of(criteria.comparisonRoots.begin(),criteria.comparisonRoots.end(),[&](const auto& root) { return VideoScanner::IsWithinFolder(file.fullPath,root); })) return false;
        for(const auto& root:criteria.excludedFolders) if(VideoScanner::IsWithinFolder(file.fullPath,root)) return false;
        for(const auto& root:criteria.protectedFolders) if(VideoScanner::IsWithinFolder(file.fullPath,root)) file.protectedFile=file.protectedByFolder=true;
        restored.push_back(std::move(file));
    }
    files=std::move(restored); return true;
}
}
