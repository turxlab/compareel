#include "MainWindow.h"
#include "Language.h"
#include "ScanCache.h"
#include "ImageReader.h"
#include "platform/Platform.h"
#include "PathKey.h"
#include <QLabel>
#include <QApplication>
#include <QElapsedTimer>
#include <QAction>
#include <QDataStream>
#include <QFutureWatcher>
#include <QLineEdit>
#include <QStatusBar>
#include <QtConcurrent/QtConcurrentRun>
#include <QtConcurrent/QtConcurrentMap>
#include <QCoreApplication>
#include <QEventLoop>
#include <QComboBox>
#include <QSpinBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QTemporaryDir>
#include <QThread>
#include <QtEndian>
#if __has_include(<QtZlib/zlib.h>)
#include <QtZlib/zlib.h>
#else
#include <zlib.h>
#endif
#include <unordered_set>
#include <cstdio>

namespace {
constexpr qint64 maxSessionBytes=32*1024*1024;
constexpr qsizetype maxSessionFiles=100000;
constexpr uint64_t minSaveAvailableRamBytes=64ULL*1024*1024;
std::string identityKey(const FileIdentity& id) {
    if(!id.valid) return {};
    char buf[96];
    const int n=std::snprintf(buf,sizeof(buf),"%llu:%llu:%llu:%llu",
        static_cast<unsigned long long>(id.volume),static_cast<unsigned long long>(id.index),
        static_cast<unsigned long long>(id.size),static_cast<unsigned long long>(id.modified));
    return n>0 ? std::string(buf,static_cast<size_t>(n)) : std::string{};
}
QJsonArray names(const std::vector<std::wstring>& values) {
    QJsonArray result; for(const auto& value:values) result.append(QString::fromStdWString(value)); return result;
}
std::vector<std::wstring> names(const QJsonArray& values) {
    std::vector<std::wstring> result; for(auto value:values) result.push_back(value.toString().toStdWString()); return result;
}
QJsonArray identity(const FileIdentity& value) {
    return {QString::number(value.volume),QString::number(value.index),QString::number(value.size),QString::number(value.modified)};
}
bool identity(const QJsonArray& values,FileIdentity& value) {
    if(values.size()!=4) return false;
    bool ok=true,field=false;
    value.volume=values[0].toString().toULongLong(&field); ok&=field;
    value.index=values[1].toString().toULongLong(&field); ok&=field;
    value.size=values[2].toString().toULongLong(&field); ok&=field;
    value.modified=values[3].toString().toULongLong(&field); ok&=field;
    return value.valid=ok;
}
bool validFolders(const QJsonValue& value) {
    if(!value.isArray() || value.toArray().size()>4096) return false;
    for(auto item:value.toArray()) {
        auto path=item.toString();
        if(path.isEmpty() || path.size()>32767 || path.contains(QChar(0)) || !QFileInfo(path).isAbsolute()) return false;
    }
    return true;
}
}

void MainWindow::saveSession() {
    if(scanning_ || sortingMetadata_ || fileOperationPending_ || clearingCache_ || closing_) return;
    auto path=QFileDialog::getSaveFileName(this,Language::text("Save review session…"),{},"Compareel session (*.cvsession)");
    SessionSaveStatus status=SessionSaveStatus::WriteFailed;
    if(!path.isEmpty() && !saveSessionFile(path,&status)) {
        const char* reason=status==SessionSaveStatus::LimitExceeded
            ?"The session exceeds the 100,000-file or 32 MiB snapshot limit. Any previous save is kept."
            :status==SessionSaveStatus::LowMemory
            ?"Available memory is too low to save the session safely. Any previous save is kept."
            :"The session could not be written. Check the destination folder permissions. Any previous save is kept.";
        QMessageBox::warning(this,Language::text("Save review session…"),Language::text(reason));
    }
}
void MainWindow::loadSession() {
    if(scanning_ || sortingMetadata_ || fileOperationPending_ || clearingCache_ || closing_) return;
    auto path=QFileDialog::getOpenFileName(this,Language::text("Open review session…"),{},"Compareel session (*.cvsession)");
    if(path.isEmpty() || loadSessionFile(path)) return;
    if(legacySessionNeedsScan_) {
        if(QMessageBox::question(this,Language::text("Open review session…"),Language::text("This older session saved the file list but not the results. Rebuild the results once? This will read the files again. Save a new session afterwards for direct reopening."),QMessageBox::Yes|QMessageBox::No,QMessageBox::No)==QMessageBox::Yes)
            loadSessionFile(path,true);
    } else QMessageBox::warning(this,Language::text("Open review session…"),Language::text("This session is invalid or uses an unsupported version."));
}
bool MainWindow::saveSessionFile(const QString& path,SessionSaveStatus* status) {
    auto shared=std::make_shared<SessionSaveStatus>(SessionSaveStatus::WriteFailed);
    auto job=sessionSaveJob(path,{},shared);
    const bool ok=job && job();
    if(status) *status=ok?SessionSaveStatus::Saved:*shared;
    return ok;
}
std::function<bool()> MainWindow::sessionSaveJob(const QString& path,std::shared_ptr<std::atomic<bool>> cancelled,std::shared_ptr<SessionSaveStatus> status) {
    auto setStatus=[&](SessionSaveStatus s)->std::function<bool()> { if(status) *status=s; return {}; };
    if(scanning_ || sortingMetadata_ || fileOperationPending_ || clearingCache_ || closing_) return setStatus(SessionSaveStatus::BusyOrInvalid);
    if(filter_->text().size()>4096) return setStatus(SessionSaveStatus::BusyOrInvalid);
    const uint64_t availRam=Platform::AvailablePhysicalMemoryBytes(UINT64_MAX);
    if(simulateLowMemorySave_ || (files_.size()>1000 && availRam<minSaveAvailableRamBytes)) return setStatus(SessionSaveStatus::LowMemory);
    struct SavedFile { std::wstring path; FileIdentity id; bool protectedFile,folderProtected; int root; HRESULT error; bool corrupted,previewFailed; uint64_t createdTime; std::shared_ptr<VideoFileInfo> evidence; };
    std::vector<SavedFile> snapshot;
    QJsonArray savedGroups;
    std::vector<std::pair<std::wstring,std::wstring>> rightCandidatePairs;
    QJsonArray marks;
    try {
        snapshot.reserve(files_.size());
        QSet<int> resultMembers;
        for(const auto& group:groups_) for(int index:group.fileIndices) resultMembers.insert(index);
        std::vector<int> remap(files_.size(),-1);
        for(int i=0;i<int(files_.size());++i) {
            const auto& file=files_[i]; if(file.deleted || !file.identity.valid) continue;
            remap[i]=int(snapshot.size());
            std::shared_ptr<VideoFileInfo> evidence;
            if(resultMembers.contains(i)) { evidence=std::make_shared<VideoFileInfo>(file); std::vector<uint8_t>().swap(evidence->thumbBGRA); }
            snapshot.push_back({file.fullPath,file.identity,file.protectedFile,file.protectedByFolder,file.comparisonRoot,file.error,file.corrupted,file.previewFailed,file.createdTime,std::move(evidence)});
        }
        if(snapshot.size()>maxSessionFiles) return setStatus(SessionSaveStatus::LimitExceeded);
        if(manual_ && snapshot.size()!=2) return setStatus(SessionSaveStatus::BusyOrInvalid);
        for(const auto& group:groups_) {
            QJsonArray members; int selected=-1;
            for(int slot=0;slot<int(group.fileIndices.size());++slot) {
                const int mapped=remap[group.fileIndices[slot]]; if(mapped<0) continue;
                if(slot==group.rightSlot) selected=int(members.size());
                members.append(mapped);
            }
            if(members.size()>=2) {
                QJsonObject row{{"members",members},{"right",selected>0?selected:1}};
                if(group.manualReference && remap[group.fileIndices[0]]>=0) row.insert("manualReference",true);
                savedGroups.append(row);
            }
            if(group.fileIndices.size()>2 && group.rightSlot>=1 && group.rightSlot<static_cast<int>(group.fileIndices.size())) {
                rightCandidatePairs.emplace_back(files_[group.fileIndices[0]].fullPath,
                                                 files_[group.fileIndices[group.rightSlot]].fullPath);
            }
        }
        for(int index:markedFiles_) if(index>=0 && index<int(files_.size()) && !files_[index].deleted) {
            auto key=FileReviewId(files_[index]);
            if(!key.empty()) marks.append(QString::fromStdString(key));
        }
    } catch(const std::bad_alloc&) {
        return setStatus(SessionSaveStatus::LowMemory);
    }
    const auto& savedCriteria=manual_?options_:scanCriteria_;
    QJsonObject criteria{{"size",savedCriteria.matchSize},{"duration",options_.images?false:savedCriteria.matchDuration},{"resolution",savedCriteria.matchResolution},{"first",savedCriteria.matchFirstFrame},
        {"fileName",savedCriteria.matchFileName},{"extension",savedCriteria.matchExtension},
        {"minimum",QString::number(savedCriteria.minFileSize)},{"maximum",QString::number(savedCriteria.maxFileSize)},
        {"transforms",savedCriteria.matchTransforms},{"crossFolders",savedCriteria.crossFolderOnly},{"threshold",savedCriteria.minFrameSimilarity},{"tolerance",QString::number(savedCriteria.durationTol100ns)},
        {"extensions",names(savedCriteria.extensions)},{"excludedExtensions",names(savedCriteria.excludedExtensions)},
        {"nonRecursiveFolders",names(savedCriteria.nonRecursiveFolders)},{"recursive",savedCriteria.includeSubfolders},{"sort",int(savedCriteria.sortOrder)}};
    QJsonObject view{{"left",left_->fileIndex>=0?QString::fromStdWString(files_[left_->fileIndex].fullPath):QString()},{"right",right_->fileIndex>=0?QString::fromStdWString(files_[right_->fileIndex].fullPath):QString()},
        {"leftTime",QString::number(left_->player->GetPosition100ns())},{"rightTime",QString::number(right_->player->GetPosition100ns())},
        {"zoom",left_->canvas->zoom()},{"panX",left_->canvas->pan().x()},{"panY",left_->canvas->pan().y()},{"offset",alignmentOffset100ns_/10000000.0},{"mode",right_->canvas->comparisonMode()},
        {"linked",linkAction_->isChecked()},{"filter",filter_->text()},{"marked",marks},
        {"referenceRule",referenceRule_->currentData().toInt()},{"evidenceFilter",evidenceFilter_->currentIndex()}};
    QJsonArray savedIssues;
    for(size_t i=0;i<std::min<size_t>(scanIssues_.size(),size_t(maxSessionFiles));++i) {
        const auto& issue=scanIssues_[i];
        savedIssues.append(QJsonObject{{"path",QString::fromStdWString(issue.path)},{"error",QString::number(quint32(issue.error))}});
    }
    QJsonObject document{{"format","Compareel.session"},{"version",3},{"images",options_.images},{"criteria",criteria},{"view",view},{"groups",savedGroups},
        {"issues",savedIssues},{"elapsedMs",QString::number(lastScanElapsedMs_)},{"outcome",lastScanOutcome_},
        {"manual",manual_},{"profile",manual_?profile_->currentIndex():scanCriteria_.matchContent?0:(scanCriteria_.matchVisualSamples || (scanCriteria_.images && scanCriteria_.matchFirstFrame && !scanCriteria_.matchSize && !scanCriteria_.matchResolution))?1:2},{"folders",names(folders_)},
        {"excluded",names(options_.excludedFolders)},{"protectedFolders",names(options_.protectedFolders)}};
    if(scanMetrics_.compactedFiles || scanMetrics_.discoveredFiles>0) {
        document.insert("scanSummary",QJsonObject{
            {"compacted",scanMetrics_.compactedFiles},
            {"discovered",QString::number(qulonglong(scanMetrics_.discoveredFiles))},
            {"outsideLimits",QString::number(qulonglong(scanMetrics_.outsideSizeLimits))},
            {"uniqueSize",QString::number(qulonglong(scanMetrics_.skippedUniqueSize))}
        });
    }
    const bool simulateLowMem=simulateLowMemorySave_;
    return [snapshot=std::move(snapshot),rightCandidatePairs=std::move(rightCandidatePairs),document=std::move(document),cachePath=cachePath_,path,cancelled,simulateLowMem,status]() mutable {
    auto fail=[&](SessionSaveStatus s) { if(status) *status=s; return false; };
    try {
    if(cancelled && cancelled->load()) return fail(SessionSaveStatus::BusyOrInvalid);
    if(simulateLowMem || (snapshot.size()>1000 && Platform::AvailablePhysicalMemoryBytes(UINT64_MAX)<minSaveAvailableRamBytes)) return fail(SessionSaveStatus::LowMemory);
    QJsonObject rightCandidates;
    for(const auto& [leftPath,rightPath]:rightCandidatePairs) {
        rightCandidates.insert(QString::fromStdWString(leftPath),QString::fromStdWString(rightPath));
    }
    auto viewObj=document["view"].toObject();
    viewObj.insert("rightCandidates",rightCandidates);
    document.insert("view",viewObj);

    ScanCache cache(cachePath);
    const auto ignored=cache.ignoredPairs();
    if(cancelled && cancelled->load()) return fail(SessionSaveStatus::BusyOrInvalid);
    const auto reviewMap=cache.reviewDecisions();
    if(!cache.error().isEmpty() || (cancelled && cancelled->load())) return fail(SessionSaveStatus::WriteFailed);

    const bool needIdentities=!ignored.empty() || !reviewMap.empty();
    std::unordered_set<std::string> identities;
    if(needIdentities) identities.reserve(snapshot.size()*2);

    // Encoding and compressing evidence dominates large saves; each file is independent, so spread it across cores.
    std::vector<size_t> withEvidence;
    for(size_t i=0;i<snapshot.size();++i) if(snapshot[i].evidence) withEvidence.push_back(i);
    std::vector<QString> evidence(snapshot.size()); std::atomic<bool> oversized{false};
    QtConcurrent::blockingMap(withEvidence,[&](size_t i) {
        if(oversized.load() || (cancelled && cancelled->load())) return;
        const auto payload=ScanCache::encodeEvidence(*snapshot[i].evidence);
        if(payload.size()>131072) { oversized.store(true); return; }
        evidence[i]=QString::fromLatin1(qCompress(payload,1).toBase64());
    });
    if(oversized.load()) return fail(SessionSaveStatus::LimitExceeded);
    if(cancelled && cancelled->load()) return fail(SessionSaveStatus::BusyOrInvalid);
    QJsonArray files;
    size_t count=0;
    for(const auto& file:snapshot) {
        if(((++count)&4095)==0 && cancelled && cancelled->load()) return fail(SessionSaveStatus::BusyOrInvalid);
        QJsonObject row{{"path",QString::fromStdWString(file.path)},{"identity",identity(file.id)},{"protected",file.protectedFile},{"folderProtected",file.folderProtected},
            {"root",file.root},{"error",QString::number(quint32(file.error))},{"corrupted",file.corrupted}};
        if(file.previewFailed) row.insert("previewFailed",true);
        if(file.createdTime>0) row.insert("created",QString::number(file.createdTime));
        if(file.evidence) row.insert("evidence",std::move(evidence[count-1]));
        files.append(row);
        if(needIdentities) {
            identities.insert(identityKey(file.id));
            if(!HasUniqueFileIndex(file.id)) identities.insert(FileReviewId(file.id,Platform::NormalizePathKey(file.path)));
        }
    }
    QJsonArray decisions;
    for(const auto& pair:ignored) {
        auto separator=pair.find('/');
        if(separator!=std::string::npos && identities.contains(pair.substr(0,separator)) && identities.contains(pair.substr(separator+1))) decisions.append(QString::fromStdString(pair));
    }
    QJsonObject reviews;
    for(const auto& [pair,decision]:reviewMap) {
        const auto separator=pair.find('/');
        if(separator!=std::string::npos && identities.contains(pair.substr(0,separator)) && identities.contains(pair.substr(separator+1))) reviews.insert(QString::fromStdString(pair),int(decision));
    }
    if(decisions.size()>100000 || reviews.size()>100000) return fail(SessionSaveStatus::LimitExceeded);
    if(cancelled && cancelled->load()) return fail(SessionSaveStatus::BusyOrInvalid);
    document.insert("files",files); document.insert("ignoredPairs",decisions); document.insert("reviewDecisions",reviews);
    auto bytes=QJsonDocument(document).toJson(QJsonDocument::Compact);
    // Reject an unreadable session before opening its destination, preserving any previous save.
    if(bytes.size()>maxSessionBytes) return fail(SessionSaveStatus::LimitExceeded);
    if(cancelled && cancelled->load()) return fail(SessionSaveStatus::BusyOrInvalid);
    QSaveFile output(path); if(!output.open(QIODevice::WriteOnly)) return fail(SessionSaveStatus::WriteFailed);
    if(output.write(bytes)==bytes.size() && !(cancelled && cancelled->load()) && output.commit()) {
        if(status) *status=SessionSaveStatus::Saved;
        return true;
    }
    return fail((cancelled && cancelled->load())?SessionSaveStatus::BusyOrInvalid:SessionSaveStatus::WriteFailed);
    } catch(const std::bad_alloc&) { return fail(SessionSaveStatus::LowMemory); }
      catch(...) { return fail(SessionSaveStatus::WriteFailed); }
    };
}
bool MainWindow::saveLastReview(bool force) {
    // Hiding all previews pauses the automatic last-review file, which records paths; an explicit Save review session still works.
    if(!rememberReviewAction_->isChecked() || privacyAction_->isChecked() || reviewSaving_ || scanning_ || sortingMetadata_ || fileOperationPending_ || clearingCache_ || closing_ || currentGroup_<0 || restorePositions_) return false;
    QByteArray token; QDataStream state(&token,QIODevice::WriteOnly);
    const auto quantum=force?1:300000000;
    state<<quint64(generation_)<<currentGroup_<<left_->fileIndex<<right_->fileIndex<<left_->canvas->zoom()<<left_->canvas->pan()
         <<qint64(alignmentOffset100ns_)<<right_->canvas->comparisonMode()<<linkAction_->isChecked()<<filter_->text()
         <<qint64(left_->player->GetPosition100ns()/quantum)<<qint64(right_->player->GetPosition100ns()/quantum)
         <<left_->player->IsPlaying()<<right_->player->IsPlaying();
    if(!reviewDirty_ && token==lastReviewToken_) return false;
    auto cancelled=std::make_shared<std::atomic<bool>>(false);
    auto status=std::make_shared<SessionSaveStatus>(SessionSaveStatus::WriteFailed);
    auto job=sessionSaveJob(lastReviewPath_,cancelled,status);
    if(!job) {
        if(*status==SessionSaveStatus::LimitExceeded) {
            lastReviewToken_=token; reviewDirty_=false;
            statusBar()->showMessage(Language::text("Automatic review save skipped: result set exceeds the 100,000-file or 32 MiB session limit."));
        }
        return false;
    }
    reviewSaveCancel_=cancelled; reviewSaving_=true; reviewDirty_=false;
    auto watcher=new QFutureWatcher<bool>(this);
    reviewSaveWatcher_=watcher;
    connect(watcher,&QFutureWatcherBase::finished,this,[this,watcher,token,cancelled,status] {
        const bool saved=watcher->result(); watcher->deleteLater(); reviewSaveWatcher_=nullptr; reviewSaving_=false;
        if(saved) lastReviewToken_=token;
        else if(cancelled->load()) reviewDirty_=true;
        else if(*status==SessionSaveStatus::LimitExceeded) {
            lastReviewToken_=token;
            statusBar()->showMessage(Language::text("Automatic review save skipped: result set exceeds the 100,000-file or 32 MiB session limit."));
        } else statusBar()->showMessage(Language::text("The last review could not be saved. Any previous saved review is kept."));
        resumeReviewAction_->setEnabled(QFileInfo::exists(lastReviewPath_));
        if(pendingClose_) close();
    });
    watcher->setFuture(QtConcurrent::run(std::move(job))); return true;
}
bool MainWindow::loadSessionFile(const QString& path,bool rebuildLegacy) {
    legacySessionNeedsScan_=false;
    if(scanning_ || sortingMetadata_ || fileOperationPending_ || clearingCache_ || closing_) return false;
    QFile input(path); if(!input.open(QIODevice::ReadOnly) || input.size()>maxSessionBytes) return false;
    auto document=QJsonDocument::fromJson(input.readAll()); if(!document.isObject()) return false;
    auto object=document.object(); const auto format=object["format"].toString();
    const int version=object["version"].toInt();
    if(format!="Compareel.session" || (version!=1 && version!=2 && version!=3)) return false;
    const bool snapshot=version==3;
    const bool images=snapshot?object["images"].toBool():version==2;
    if(snapshot && (!object["images"].isBool() || !object["groups"].isArray())) return false;
    if(images && (!object["images"].isBool() || !object["images"].toBool())) return false;
    if(version==1 && object["images"].toBool()) return false;
    if(!object["files"].isArray() || !object["criteria"].isObject() || !object["view"].isObject() || !object["manual"].isBool() || !object["ignoredPairs"].isArray()) return false;
    for(auto field:{"folders","excluded","protectedFolders"}) if(!validFolders(object[field])) return false;
    auto saved=object["files"].toArray(); if(saved.size()>maxSessionFiles || (object["manual"].toBool() && saved.size()!=2)) return false;
    std::vector<VideoFileInfo> files; files.reserve(saved.size());
    QSet<QString> seen,physicalFiles; seen.reserve(saved.size()); physicalFiles.reserve(saved.size());
    for(auto item:saved) {
        auto row=item.toObject(); QString name=row["path"].toString();
        if(name.isEmpty() || name.size()>32767 || name.contains(QChar(0)) || !QFileInfo(name).isAbsolute()) return false;
        auto normalized=QDir::toNativeSeparators(QDir::cleanPath(name));
        if(seen.contains(pathKey(normalized))) return false; seen.insert(pathKey(normalized));
        VideoFileInfo file; if(!identity(row["identity"].toArray(),file.identity)) return false;
        if(HasUniqueFileIndex(file.identity)) {
            auto physical=QString::number(file.identity.volume)+":"+QString::number(file.identity.index);
            if(physicalFiles.contains(physical)) return false;
            physicalFiles.insert(physical);
        }
        file.fullPath=normalized.toStdWString(); file.fileName=QFileInfo(name).fileName().toStdWString(); file.folderPath=QFileInfo(name).absolutePath().toStdWString();
        if(images ? !VideoScanner::IsImageExtension(file.fullPath) : VideoScanner::IsImageExtension(file.fullPath)) return false;
        file.fileSize=file.identity.size; file.protectedByFolder=row["folderProtected"].toBool(); file.protectedFile=row["protected"].toBool() || file.protectedByFolder; files.push_back(std::move(file));
        if(snapshot) {
            auto& loaded=files.back(); bool errorOk=false;
            const auto error=row["error"].toString().toULongLong(&errorOk);
            if(!errorOk || error>UINT32_MAX || !row["corrupted"].isBool()) return false;
            loaded.error=HRESULT(uint32_t(error)); loaded.corrupted=row["corrupted"].toBool();
            loaded.previewFailed=row["previewFailed"].toBool(false);
            if(row.contains("created")) {
                bool cOk=false; const auto cVal=row["created"].toString().toULongLong(&cOk);
                if(!cOk) return false;
                loaded.createdTime=cVal;
            }
            loaded.comparisonRoot=row["root"].toInt(-1);
            if(loaded.comparisonRoot<-1 || loaded.comparisonRoot>=object["folders"].toArray().size()) return false;
            if(row.contains("evidence")) {
                const auto packed=QByteArray::fromBase64(row["evidence"].toString().toLatin1(),QByteArray::AbortOnBase64DecodingErrors);
                if(packed.size()<4 || qFromBigEndian<quint32>(packed.constData())>131072) return false;
                QByteArray payload(qFromBigEndian<quint32>(packed.constData()),Qt::Uninitialized);
                uLongf length=payload.size();
                if(payload.isEmpty() || uncompress(reinterpret_cast<Bytef*>(payload.data()),&length,reinterpret_cast<const Bytef*>(packed.constData()+4),uLong(packed.size()-4))!=Z_OK || length!=uLongf(payload.size()) || !ScanCache::decodeEvidence(payload,loaded,true)) return false;
            }
        }
    }
    std::vector<VideoGroup> savedGroups;
    std::vector<ScanIssue> savedIssues;
    if(snapshot) {
        if(!object["issues"].isArray() || object["issues"].toArray().size()>maxSessionFiles) return false;
        for(auto entry:object["issues"].toArray()) {
            auto issue=entry.toObject(); const auto name=issue["path"].toString(); bool ok=false;
            const auto error=issue["error"].toString().toULongLong(&ok);
            if(name.isEmpty() || name.size()>32767 || name.contains(QChar(0)) || !QFileInfo(name).isAbsolute() || !ok || error>UINT32_MAX) return false;
            savedIssues.push_back({name.toStdWString(),HRESULT(uint32_t(error))});
        }
        const auto rows=object["groups"].toArray(); if(rows.size()>maxSessionFiles) return false;
        qsizetype totalMembers=0;
        for(auto item:rows) {
            if(!item.isObject()) return false;
            auto row=item.toObject(); if(!row["members"].isArray()) return false;
            const auto members=row["members"].toArray(); totalMembers+=members.size();
            if(members.size()<2 || totalMembers>maxSessionFiles*4) return false;
            VideoGroup group; QSet<int> seenMembers;
            for(auto member:members) {
                const int index=member.toInt(-1);
                if(!member.isDouble() || member.toDouble()!=index || index<0 || index>=int(files.size()) || seenMembers.contains(index) || !saved[index].toObject().contains("evidence")) return false;
                seenMembers.insert(index); group.fileIndices.push_back(index);
            }
            group.rightSlot=row["right"].toInt(-1); group.manualReference=row["manualReference"].toBool(false);
            if(group.rightSlot<1 || group.rightSlot>=int(group.fileIndices.size())) return false;
            savedGroups.push_back(std::move(group));
        }
    }
    ScanMetrics savedMetrics;
    if(snapshot && object.contains("scanSummary")) {
        if(!object["scanSummary"].isObject()) return false;
        const auto s=object["scanSummary"].toObject();
        bool dOk=false,oOk=false,uOk=false;
        const auto dVal=s["discovered"].toString("0").toULongLong(&dOk);
        const auto oVal=s["outsideLimits"].toString("0").toULongLong(&oOk);
        const auto uVal=s["uniqueSize"].toString("0").toULongLong(&uOk);
        if(!dOk || !oOk || !uOk || (s.contains("compacted") && !s["compacted"].isBool())) return false;
        savedMetrics.compactedFiles=s["compacted"].toBool(false);
        savedMetrics.discoveredFiles=size_t(dVal);
        savedMetrics.outsideSizeLimits=size_t(oVal);
        savedMetrics.skippedUniqueSize=size_t(uVal);
    }
    ScanCriteria criteria=options_; criteria.images=images; auto c=object["criteria"].toObject(); bool minimum=false,maximum=false,tolerance=false;
    criteria.minFileSize=c["minimum"].toString().toULongLong(&minimum); criteria.maxFileSize=c["maximum"].toString().toULongLong(&maximum);
    criteria.durationTol100ns=c["tolerance"].toString().toLongLong(&tolerance); criteria.minFrameSimilarity=c["threshold"].toDouble(-1);
    int sort=c["sort"].toInt(-1),profile=object["profile"].toInt(-1);
    if(!minimum || !maximum || !tolerance || criteria.minFileSize>criteria.maxFileSize || criteria.durationTol100ns<0 || criteria.durationTol100ns>600000000 || criteria.minFrameSimilarity<70 || criteria.minFrameSimilarity>100 || sort<0 || sort>8 || profile<0 || profile>2) return false;
    criteria.matchTransforms=c["transforms"].toBool(false); criteria.crossFolderOnly=c["crossFolders"].toBool(false);
    if(criteria.crossFolderOnly && object["folders"].toArray().size()<2) return false;
    criteria.matchSize=c["size"].toBool(); criteria.matchDuration=c["duration"].toBool(); criteria.matchResolution=c["resolution"].toBool(); criteria.matchFirstFrame=c["first"].toBool();
    criteria.matchFileName=c["fileName"].toBool(false); criteria.matchExtension=c["extension"].toBool(false);
    if(images && criteria.matchDuration) return false;
    if(images && (sort==int(SortOrder::DurationAsc) || sort==int(SortOrder::DurationDesc))) return false;
    if(profile==2 && !criteria.matchSize && !criteria.matchDuration && !criteria.matchResolution && !criteria.matchFirstFrame && !criteria.matchFileName && !criteria.matchExtension) return false;
    criteria.sortOrder=static_cast<SortOrder>(sort); criteria.includeSubfolders=c["recursive"].toBool(); criteria.extensions=names(c["extensions"].toArray());
    if(criteria.extensions.size()>100) return false;
    for(const auto& extension:criteria.extensions) if(extension.size()<2 || extension.size()>32 || extension.front()!=L'.' || extension.find_first_of(L"/\\:;\0",0,5)!=std::wstring::npos) return false;
    criteria.excludedExtensions=names(c["excludedExtensions"].toArray());
    if(criteria.excludedExtensions.size()>100) return false;
    for(const auto& extension:criteria.excludedExtensions) if(extension.size()<2 || extension.size()>32 || extension.front()!=L'.' || extension.find_first_of(L"/\\:;\0",0,5)!=std::wstring::npos) return false;
    if(c.contains("nonRecursiveFolders")) {
        if(!validFolders(c["nonRecursiveFolders"])) return false;
        criteria.nonRecursiveFolders=names(c["nonRecursiveFolders"].toArray());
    } else {
        criteria.nonRecursiveFolders.clear();
    }
    criteria.excludedFolders=names(object["excluded"].toArray()); criteria.protectedFolders=names(object["protectedFolders"].toArray());
    // Folder protection is authoritative even if per-file flags were edited or omitted.
    for(auto& file:files) for(const auto& root:criteria.protectedFolders)
        if(VideoScanner::IsWithinFolder(file.fullPath,root)) file.protectedFile=file.protectedByFolder=true;
    auto view=object["view"].toObject(); bool leftTime=false,rightTime=false;
    // Sessions before the reference rule stored a left-file preference (0..4); only "largest file" (1) carries over.
    const int legacyLeft=view["leftPreference"].toInt(0),evidenceFilter=view["evidenceFilter"].toInt(0);
    if(legacyLeft<0 || legacyLeft>4 || evidenceFilter<0 || evidenceFilter>=evidenceFilter_->count()) return false;
    const int referenceRule=view.contains("referenceRule")?view["referenceRule"].toInt(-1):legacyLeft==1?int(ReferenceRule::LargestFile):int(ReferenceRule::HighestResolution);
    if(referenceRule<int(ReferenceRule::HighestResolution) || referenceRule>int(ReferenceRule::OldestCaptureDate)) return false;
    bool elapsedOk=false; const auto elapsed=object["elapsedMs"].toString().toLongLong(&elapsedOk);
    const auto outcome=object["outcome"].toString();
    if(snapshot && (!elapsedOk || elapsed<0 || (!outcome.isEmpty() && !QStringList{"completed","stopped","paused","failed"}.contains(outcome)))) return false;
    if((view.contains("linked") && !view["linked"].isBool()) || (view.contains("filter") && (!view["filter"].isString() || view["filter"].toString().size()>4096))) return false;
    auto left=view["leftTime"].toString().toLongLong(&leftTime),right=view["rightTime"].toString().toLongLong(&rightTime);
    double zoom=view["zoom"].toDouble(-1),x=view["panX"].toDouble(),y=view["panY"].toDouble();
    double offset=view["offset"].toDouble(); if(!std::isfinite(offset) || std::abs(offset)>86400) return false;
    int mode=view["mode"].toInt(); if(mode<0 || mode>3) return false;
    if(!leftTime || !rightTime || left<0 || right<0 || zoom<0.5 || zoom>8 || std::abs(x)>1000000 || std::abs(y)>1000000) return false;
    std::unordered_set<std::string> identities;
    identities.reserve(files.size()*2);
    for(const auto& file:files) {
        identities.insert(identityKey(file.identity));
        if(!HasUniqueFileIndex(file.identity)) identities.insert(FileReviewId(file));
    }
    std::set<std::string> decisions;
    std::set<std::string> marks;
    if(view.contains("marked")) {
        if(!view["marked"].isArray() || view["marked"].toArray().size()>100000) return false;
        for(const auto& entry:view["marked"].toArray()) {
            const auto key=entry.toString().toStdString(); if(!entry.isString() || !identities.contains(key)) return false; marks.insert(key);
        }
    }
    auto savedDecisions=object["ignoredPairs"].toArray(); if(savedDecisions.size()>100000) return false;
    for(auto entry:savedDecisions) {
        auto pair=entry.toString().toStdString(); auto separator=pair.find('/');
        if(pair.size()>220 || separator==std::string::npos || !identities.contains(pair.substr(0,separator)) || !identities.contains(pair.substr(separator+1))) return false;
        decisions.insert(pair);
    }
    ReviewDecisions reviews;
    if(object.contains("reviewDecisions")) {
        if(!object["reviewDecisions"].isObject()) return false;
        const auto savedReviews=object["reviewDecisions"].toObject(); if(savedReviews.size()>100000) return false;
        for(auto entry=savedReviews.begin();entry!=savedReviews.end();++entry) {
            const auto pair=entry.key().toStdString(); const auto separator=pair.find('/'); const auto value=entry.value().toInt(-1);
            if(pair.size()>220 || separator==std::string::npos || !identities.contains(pair.substr(0,separator)) || !identities.contains(pair.substr(separator+1)) || value<1 || value>3 || entry.value().toDouble()!=value) return false;
            reviews.emplace(pair,static_cast<ReviewDecision>(value));
        }
    }
    if(!snapshot && !rebuildLegacy) { legacySessionNeedsScan_=true; return false; }
    { ScanCache cache(cachePath_); cache.mergeIgnoredPairs(decisions);
      const auto local=cache.reviewDecisions();
      // Opening a session must not silently replace an existing local decision.
      std::erase_if(reviews,[&](const auto& entry) { return local.contains(entry.first); });
      cache.saveReviewDecisions(reviews); if(!cache.error().isEmpty()) return false; }
    refreshReviewDecisions();
    options_=std::move(criteria); folders_=names(object["folders"].toArray()); updateMediaMode(); translate(); profile_->setCurrentIndex(profile);
    { QSignalBlocker sortBlock(sort_); sort_->setCurrentIndex(static_cast<int>(options_.sortOrder)); }
    { QSignalBlocker block(referenceRule_); const int found=referenceRule_->findData(referenceRule); referenceRule_->setCurrentIndex(std::max(found,0)); }
    { QSignalBlocker block(evidenceFilter_); evidenceFilter_->setCurrentIndex(evidenceFilter); }
    const char* modes[]={"Side by side","Wipe","Overlay","Difference (display aid)"};
    if(auto action=findChild<QAction*>(modes[mode])) action->trigger();
    restoreRightCandidates_.clear();
    auto normalizedPath=[](const QString& value) { return value.isEmpty()?value:QDir::toNativeSeparators(QDir::cleanPath(value)); };
    if(view.contains("rightCandidates") && view["rightCandidates"].isObject()) {
        auto rc=view["rightCandidates"].toObject();
        for(auto it=rc.begin();it!=rc.end();++it) restoreRightCandidates_.insert(normalizedPath(it.key()),normalizedPath(it.value().toString()));
    }
    restoreLeft_=normalizedPath(view["left"].toString()); restoreRight_=normalizedPath(view["right"].toString()); restoreLeftTime_=left; restoreRightTime_=right; restoreZoom_=zoom; restorePan_={x,y};
    restoreOffset_=offset;
    restoreLinked_=view["linked"].toBool(true); restoreMarks_=std::move(marks); filter_->setText(view["filter"].toString());
    restoredFiles_=std::move(files); restoreManual_=object["manual"].toBool();
    restoredGroups_=snapshot?std::optional<std::vector<VideoGroup>>(std::move(savedGroups)):std::nullopt;
    restoredIssues_=std::move(savedIssues);
    restoredMetrics_=std::move(savedMetrics);
    if(snapshot) { lastScanElapsedMs_=elapsed; lastScanOutcome_=outcome; }
    startScan(); return true;
}

bool MainWindow::runSessionSnapshotCheck() {
    auto fail=[](const char* message) { qCritical("FAIL session snapshot: %s",message); return false; };
    QTemporaryDir fixture; if(!fixture.isValid()) return fail("fixture");
    auto wait=[](const auto& ready) { QElapsedTimer t; t.start(); while(!ready() && t.elapsed()<15000) { QApplication::processEvents(); QThread::msleep(1); } return ready(); };
    reviewSaveTimer_.stop(); mediaMode_->setCurrentIndex(1); profile_->setCurrentIndex(1);
    manual_=false; folders_={fixture.path().toStdWString()}; options_.crossFolderOnly=false; scanCriteria_=activeCriteria();
    std::atomic<bool> cancel=false;
    files_.clear(); groups_.clear();
    for(int i=0;i<3;++i) {
        const auto path=fixture.filePath(QString("image-%1.png").arg(i)); QImage image(160,90,QImage::Format_RGB32);
        for(int y=0;y<image.height();++y) for(int x=0;x<image.width();++x) image.setPixel(x,y,qRgb((x*9+y*5)%256,(x*3)%256,(y*17)%256));
        if(!image.save(path)) return fail("generated PNG");
        VideoFileInfo file; file.fullPath=path.toStdWString(); file.fileName=QFileInfo(path).fileName().toStdWString(); file.folderPath=fixture.path().toStdWString();
        file.identity=FileOperations::ReadIdentity(file.fullPath); file.fileSize=file.identity.size; file.comparisonRoot=0;
        if(!StillImages::analyze(file,cancel)) return fail("fixture analysis"); files_.push_back(std::move(file));
    }
    for(int i=3;i<1299;++i) {
        const auto path=fixture.filePath(QString("image-%1.png").arg(i));
        if(!QFile::copy(QString::fromStdWString(files_[0].fullPath),path)) return fail("result fixture copy");
        auto file=files_[0]; file.fullPath=path.toStdWString(); file.fileName=QFileInfo(path).fileName().toStdWString();
        file.identity=FileOperations::ReadIdentity(file.fullPath); files_.push_back(std::move(file));
    }
    for(int i=1299;i<40003;++i) {
        VideoFileInfo file; file.fullPath=fixture.filePath(QString("unmatched-not-present-%1.png").arg(i)).toStdWString();
        file.identity={1,uint64_t(i+1000),1024,100,true}; file.fileSize=1024; file.comparisonRoot=0; files_.push_back(std::move(file));
    }
    VideoGroup group; group.fileIndices={2,0,1}; group.rightSlot=2; groups_={group}; currentGroup_=-1;
    for(int g=0,index=3;g<556;++g) {
        VideoGroup extra; for(int n=0;n<(g<184?3:2);++n) extra.fileIndices.push_back(index++);
        groups_.push_back(std::move(extra));
    }
    files_[0].protectedFile=true; markedFiles_.insert(1); populateResults(); selectGroup(0,2);
    linkAction_->setChecked(false); filter_->setText("png");
    lastScanElapsedMs_=613000; lastScanOutcome_="completed";
    const auto path=fixture.filePath("results.cvsession");
    if(!saveSessionFile(path)) return fail("save results");
    QFile input(path); if(!input.open(QIODevice::ReadOnly)) return fail("read results");
    const auto bytes=input.readAll(); auto document=QJsonDocument::fromJson(bytes).object(); input.close();
    if(document["version"].toInt()!=3 || document["groups"].toArray().size()!=557 || document["files"].toArray().size()!=40003) return fail("snapshot structure");
    const auto state=fixture.filePath("fresh-state");
    qint64 reopenMs=0;
    {
        MainWindow restored(state); restored.reviewSaveTimer_.stop(); restored.show();
        { ScanCache cache(restored.cachePath_); cache.clearScanData(); }
        QElapsedTimer elapsed; elapsed.start();
        if(!restored.loadSessionFile(path) || !wait([&] { return !restored.scanning_; })) return fail("empty-cache reopen");
        reopenMs=elapsed.elapsed();
        if(restored.files_.size()!=40003 || restored.groups_.size()!=557 || restored.groups_[0].fileIndices!=group.fileIndices || restored.groups_[0].rightSlot!=2 ||
           restored.left_->fileIndex!=2 || restored.right_->fileIndex!=1 || restored.linkAction_->isChecked() || !restored.markedFiles_.contains(1) || !restored.files_[0].protectedFile ||
           !restored.scanIssues_.empty() || restored.files_[1299].deleted || restored.cacheHits_ || restored.scanMetrics_.readMs || restored.scanMetrics_.groupMs || !restored.savedResultEvidence_ || restored.lastScanElapsedMs_!=613000) {
            return fail("restore saved results without scanning unmatched input");
        }
        if(restored.files_[0].sigFirst.dHashH!=files_[0].sigFirst.dHashH || !restored.files_[0].frameLoaded || !VideoScanner::FilesMatch(restored.files_[0],restored.files_[1],restored.scanCriteria_)) return fail("independent comparison evidence");
        auto rejected=[&](QJsonObject invalid) {
            const auto bad=fixture.filePath("invalid.cvsession"); QFile output(bad); if(!output.open(QIODevice::WriteOnly)) return false;
            output.write(QJsonDocument(invalid).toJson(QJsonDocument::Compact)); output.close();
            return !restored.loadSessionFile(bad) && !restored.scanning_ && restored.groups_.size()==557;
        };
        auto invalid=document; auto badGroups=document["groups"].toArray(); auto badGroup=badGroups[0].toObject(); badGroup["members"]=QJsonArray{0,40003}; badGroups[0]=badGroup; invalid["groups"]=badGroups;
        if(!rejected(invalid)) return fail("invalid member must preserve review");
        invalid=document; auto badFiles=document["files"].toArray(); auto badFile=badFiles[0].toObject();
        auto bomb=qCompress(QByteArray(131073,'x')); qToBigEndian<quint32>(1,bomb.data()); badFile["evidence"]=QString::fromLatin1(bomb.toBase64()); badFiles[0]=badFile; invalid["files"]=badFiles;
        if(!rejected(invalid)) return fail("bounded decompression must preserve review");
        invalid=document; invalid["version"]=2; invalid.remove("groups");
        if(!rejected(invalid) || !restored.legacySessionNeedsScan_) return fail("legacy must not silently rescan");
        if(!wait([&] { return restored.pendingPreviews_.empty() && restored.left_->canvas->frame() && restored.right_->canvas->frame(); })) return fail("lazy selected previews");

        // Imported hashes are display evidence, never sufficient authority for bulk removal.
        for(int index:{1,2}) {
            QFile changed(QString::fromStdWString(restored.files_[index].fullPath));
            if(!changed.open(QIODevice::Append) || changed.write(index==1?"AAAA":"BBBB",4)!=4) return fail("different real contents"); changed.close();
            auto& file=restored.files_[index]; file.identity=FileOperations::ReadIdentity(file.fullPath); file.fileSize=file.identity.size; file.contentHash=std::string(64,'0');
        }
        restored.scanCriteria_.matchContent=true; restored.scanCriteria_.matchFirstFrame=false;
        CleanupTarget target{1,restored.files_[1],restored.files_[2]}; restored.executeBulkCleanup({target});
        if(!wait([&] { return !restored.fileOperationPending_; }) || !QFileInfo::exists(QString::fromStdWString(target.file.fullPath)) || restored.files_[1].deleted) return fail("forged snapshot hash must not remove file");
        { ScanCache cache(restored.cachePath_); if(cache.operationHistory().empty()) return fail("revalidation reached real journal boundary"); }
        for(auto box:restored.findChildren<QMessageBox*>()) box->accept();
    }
    if(!QFile::remove(QString::fromStdWString(files_[0].fullPath))) return fail("missing fixture");
    {
        MainWindow changed(fixture.filePath("changed-state")); changed.reviewSaveTimer_.stop();
        if(!changed.loadSessionFile(path) || !wait([&] { return !changed.scanning_; }) || changed.groups_.size()!=556 || changed.scanIssues_.size()!=3) return fail("only missing/changed result members excluded");
    }
    qInfo("PASS session snapshot: 40003 inputs/557 groups/1299 results, bytes=%lld reopen_ms=%lld, empty cache/no archive scan or grouping, saved order/right choice/marks/protection/time/evidence, lazy previews, invalid index/decompression rejection, legacy no-rescan, changed/missing exclusion and real bulk forged-hash rejection",
        static_cast<long long>(bytes.size()),static_cast<long long>(reopenMs));
    return true;
}

bool MainWindow::runSessionScaleCheck() {
    auto fail=[](const char* message) { qCritical("FAIL session scale: %s",message); return false; };
    QTemporaryDir fixture;
    if(!fixture.isValid()) return fail("temporary directory");
    reviewSaveTimer_.stop();
    manual_=false; groups_.clear(); files_.clear(); folders_.clear();
    profile_->setCurrentIndex(0); scanCriteria_=activeCriteria();
    const auto missing=fixture.filePath("not-created/");
    files_.reserve(maxSessionFiles);
    for(int i=0;i<maxSessionFiles;++i) {
        VideoFileInfo file;
        file.fullPath=QDir::toNativeSeparators(missing+QString::number(i)+".mp4").toStdWString();
        file.identity={}; file.identity.valid=true; file.identity.volume=1;
        file.identity.index=i+100; file.identity.size=file.fileSize=i+1024; file.identity.modified=100;
        files_.push_back(std::move(file));
    }
    VideoGroup syntheticGroup; syntheticGroup.fileIndices={0,1,2}; syntheticGroup.rightSlot=2;
    groups_.push_back(syntheticGroup);
    currentGroup_=0;
    const auto path=fixture.filePath("large.cvsession");
    QElapsedTimer elapsed; elapsed.start();
    auto cancelToken=std::make_shared<std::atomic<bool>>(false);
    auto job=sessionSaveJob(path,cancelToken);
    if(!job) return fail("100000-file UI snapshot creation");
    const qint64 uiSnapshotUs=elapsed.nsecsElapsed()/1000;
    const qint64 uiSnapshotMs=elapsed.elapsed();
    QElapsedTimer bgTimer; bgTimer.start();
    if(!job()) return fail("100000-file background write");
    const qint64 bgWriteMs=bgTimer.elapsed();
    const qint64 saveMs=elapsed.elapsed();
    QFile saved(path); if(!saved.open(QIODevice::ReadOnly)) return fail("read saved session");
    const auto original=saved.readAll(); saved.close();
    if(original.size()>maxSessionBytes || QJsonDocument::fromJson(original).object()["files"].toArray().size()!=maxSessionFiles) return fail("saved size/count");
    auto unchanged=[&] { QFile input(path); return input.open(QIODevice::ReadOnly) && input.readAll()==original; };

    // Verify mid-write cooperative cancellation preserves previous valid save
    auto cancelledToken=std::make_shared<std::atomic<bool>>(true);
    auto cancelledJob=sessionSaveJob(path,cancelledToken);
    if(!cancelledJob || cancelledJob() || !unchanged()) return fail("cancelled save must preserve previous save");

    // Verify low-memory guard refuses save cleanly and preserves previous valid save
    simulateLowMemorySave_=true;
    const bool lowMemAccepted=saveSessionFile(path);
    simulateLowMemorySave_=false;
    if(lowMemAccepted || !unchanged()) return fail("low-memory guard must preserve previous save");

    // Verify file count and byte limits preserve previous valid save
    files_.push_back(files_.back());
    if(saveSessionFile(path) || !unchanged()) return fail("file limit must preserve previous save");
    files_.pop_back();
    const auto suffix=std::wstring(400,L'x');
    for(auto& file:files_) file.fullPath+=suffix;
    if(saveSessionFile(path) || !unchanged()) return fail("byte limit must preserve previous save");

    // Verify autosave + Clear scan cache + close concurrency preserves decisions, history, and valid snapshot
    for(auto& file:files_) file.fullPath.resize(file.fullPath.size()-suffix.size());
    {
        ScanCache cache(cachePath_);
        cache.ignorePair(files_[0],files_[1]);
        cache.saveReviewDecisions({{ReviewPairKey(files_[0].identity,files_[2].identity),ReviewDecision::KeepBoth}});
        const auto opId=cache.beginRemoval(files_[1]);
        cache.completeRemoval(opId,S_OK);
        if(!cache.error().isEmpty()) return fail("seed cache for concurrency check");
    }
    reviewDirty_=true;
    if(!saveLastReview(true)) return fail("start async saveLastReview before cache clear");
    if(auto clearAction=findChild<QAction*>("Clear scan cache")) clearAction->trigger();
    QElapsedTimer waitTimer; waitTimer.start();
    while((clearingCache_ || reviewSaving_) && waitTimer.elapsed()<15000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents,10); QThread::msleep(1);
    }
    if(clearingCache_ || reviewSaving_) return fail("concurrent autosave + cache clear did not finish");
    {
        ScanCache cache(cachePath_);
        if(!cache.error().isEmpty() || cache.ignoredPairs().empty() || cache.reviewDecisions().empty() || cache.operationHistory().empty())
            return fail("concurrent cache clear lost decisions or history");
    }
    if(!saveLastReview(true)) return fail("post-clear saveLastReview");
    waitTimer.restart();
    while(reviewSaving_ && waitTimer.elapsed()<15000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents,10); QThread::msleep(1);
    }
    if(reviewSaving_ || !QFileInfo::exists(lastReviewPath_)) return fail("post-clear saveLastReview completion");

    files_.clear(); groups_.clear(); currentGroup_=-1; elapsed.restart();
    if(!loadSessionFile(path)) return fail("100000-file load");
    const auto parseMs=elapsed.elapsed();
    while(scanning_ && elapsed.elapsed()<30000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents,20); QThread::msleep(1);
    }
    if(scanning_) { cancel_=true; return fail("missing-file validation timeout"); }
    if(files_.size()!=maxSessionFiles || scanIssues_.size()!=3 || !groups_.empty()) return fail("only saved result members need identity reconciliation");
    qInfo("PASS session scale: files=100000 bytes=%lld ui_snapshot_us=%lld ui_snapshot_ms=%lld bg_write_ms=%lld save_ms=%lld parse_ms=%lld load_total_ms=%lld; cancel/low-mem/count/byte rejection preserves previous save; concurrent autosave+cache-clear preserves decisions/history; only saved result identities checked and missing members excluded",
          static_cast<long long>(original.size()),static_cast<long long>(uiSnapshotUs),static_cast<long long>(uiSnapshotMs),
          static_cast<long long>(bgWriteMs),static_cast<long long>(saveMs),static_cast<long long>(parseMs),static_cast<long long>(elapsed.elapsed()));
    return true;
}
