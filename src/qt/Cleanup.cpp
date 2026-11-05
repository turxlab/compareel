#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "Language.h"
#include "Icons.h"
#include "Theme.h"
#include "FileOperations.h"
#include "platform/Platform.h"
#include "ScanCache.h"
#include "Csv.h"
#include "ImageReader.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QEventLoop>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPixmapCache>
#include <QPointer>
#include <QProgressDialog>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollBar>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

using Language::text;

void MainWindow::deleteFocusedFile() {
    auto focus=QApplication::focusWidget();
    PlayerPanel* panel=focus && left_->isAncestorOf(focus)?left_:focus && right_->isAncestorOf(focus)?right_:nullptr;
    if(!panel) { statusBar()->showMessage(text("Click the left or right preview first, then press Delete.")); return; }
    if(panel->fileIndex<0 || scanning_ || fileOperationPending_ || closing_) return;
    const auto& file=files_[panel->fileIndex];
    if(file.protectedFile || protectedByReview(file)) { statusBar()->showMessage(text("Protected")); return; }
    // The keyboard path asks first; the visible trash button keeps its one-click behaviour.
    if(QMessageBox::question(this,text("Move to Recycle Bin"),text("Move this file to the Recycle Bin?")+"\n\n"+QString::fromStdWString(file.fullPath),QMessageBox::Yes|QMessageBox::Cancel,QMessageBox::Cancel)!=QMessageBox::Yes) return;
    deleteFile(panel);
}

void MainWindow::toggleRightMark() {
    const int index=right_->fileIndex;
    // Files held by a keep or review-later decision are skipped by the cleanup, so they cannot be marked either.
    if(scanning_ || fileOperationPending_ || currentGroup_<0 || index<0 || files_[index].protectedFile || protectedByReview(files_[index])) { updateReviewState(); return; }
    if(markedFiles_.contains(index)) markedFiles_.remove(index); else markedFiles_.insert(index);
    reviewDirty_=true; updateGroupRow(currentGroup_); updateReviewState();
    statusBar()->showMessage(text(markedFiles_.contains(index)?"Marked for cleanup: %1":"Cleanup mark removed: %1").arg(QString::fromStdWString(files_[index].fileName)));
}

void MainWindow::addTransferActions(QMenu* menu,int index) {
    const bool available=!scanning_ && !sortingMetadata_ && !fileOperationPending_ && !closing_ && index>=0 && index<int(files_.size()) && !files_[index].deleted;
    for(const auto& entry:std::array<std::pair<const char*,const char*>,3>{{{"Copy to folder…","copy"},{"Move to folder…","move"},{"Quarantine…","quarantine"}}}) {
        auto item=menu->addAction(text(entry.first)); item->setObjectName(entry.second);
        item->setEnabled(available && (QString(entry.second)=="copy" || (!files_[index].protectedFile && !protectedByReview(files_[index]))));
        connect(item,&QAction::triggered,this,[this,index,operation=QString(entry.second)] { chooseTransfer(index,operation); });
    }
}
void MainWindow::chooseTransfer(int index,const QString& operation) {
    if(scanning_ || sortingMetadata_ || fileOperationPending_ || clearingCache_ || closing_ || index<0 || index>=int(files_.size())) return;
    const auto file=files_[index];
    if(file.deleted || (operation!="copy" && (file.protectedFile || protectedByReview(file)))) return;
    const auto title=text(operation=="copy"?"Copy to folder…":operation=="move"?"Move to folder…":"Quarantine…");
    const auto folder=QFileDialog::getExistingDirectory(this,title,settings_.value("transferFolder").toString()); if(folder.isEmpty()) return;
    QString directory=folder;
    if(operation=="quarantine") directory=QDir(folder).filePath("Compareel-"+QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss")+"-"+QUuid::createUuid().toString(QUuid::WithoutBraces).left(8));
    const auto destination=QDir(directory).filePath(QString::fromStdWString(file.fileName));
    const auto explanation=text(operation=="copy"?"Create a verified copy? Existing files are never overwritten.":"Create a verified copy at the destination, then move the source to Recycle Bin? Existing files are never overwritten.");
    if(QMessageBox::question(this,title,explanation+"\n\n"+QString::fromStdWString(file.fullPath)+"\n→ "+QDir::toNativeSeparators(destination),QMessageBox::Yes|QMessageBox::No,QMessageBox::No)!=QMessageBox::Yes) return;
    if(operation=="quarantine" && !QDir().mkpath(directory)) { QMessageBox::warning(this,title,text("The destination folder could not be created.")); return; }
    settings_.setValue("transferFolder",folder);
    startTransfer(file,destination,operation);
}
void MainWindow::updateUndoAction() {
    ScanCache cache(cachePath_); const auto record=cache.lastUndoableTransfer();
    undoTransferAction_->setEnabled(record.has_value());
    undoTransferAction_->setToolTip(record?record->source+"\n→ "+record->destination:text("No confirmed transfer is available to undo."));
    if(!cache.error().isEmpty()) cacheError_=cache.error();
}
void MainWindow::undoLastTransfer() {
    if(scanning_ || sortingMetadata_ || fileOperationPending_ || clearingCache_ || closing_) return;
    ScanCache cache(cachePath_); const auto record=cache.lastUndoableTransfer();
    if(!record) { updateUndoAction(); return; }
    if(QMessageBox::question(this,text("Undo last copy or move…"),text("Undo this transfer? A moved file returns to its original path; a copied file goes to Recycle Bin. Changed or occupied paths are refused.")+"\n\n"+record->source+"\n← "+record->destination,QMessageBox::Yes|QMessageBox::No,QMessageBox::No)!=QMessageBox::Yes) return;
    VideoFileInfo source; source.fullPath=record->source.toStdWString(); source.identity=record->sourceIdentity;
    startTransfer(source,record->destination,record->operation,&*record);
}
void MainWindow::startTransfer(const VideoFileInfo& file,const QString& destination,const QString& operation,const TransferRecord* undo) {
    if(scanning_ || sortingMetadata_ || fileOperationPending_ || clearingCache_ || closing_) return;
    if(!undo) {
        const auto found=std::find_if(files_.begin(),files_.end(),[&](const auto& value) { return !value.deleted && value.fullPath==file.fullPath && value.identity==file.identity; });
        if(found==files_.end() || (operation!="copy" && (found->protectedFile || protectedByReview(*found)))) return;
        if(FileOperations::ReadIdentity(file.fullPath)!=file.identity) { QMessageBox::warning(this,text("File operation failed"),text("This file changed or is no longer available. Rescan before removing it.")); return; }
        if(!beginCleanupRecovery()) return;
    } else {
        VideoFileInfo target; target.fullPath=destination.toStdWString(); target.identity=undo->destinationIdentity;
        bool protectedTarget=protectedByReview(target);
        for(const auto& root:options_.protectedFolders) protectedTarget|=VideoScanner::IsWithinFolder(target.fullPath,root);
        for(const auto& value:files_) if(!value.deleted && value.identity==target.identity) protectedTarget|=value.protectedFile;
        if(protectedTarget) { QMessageBox::warning(this,text("File operation failed"),text("Protected")); return; }
        settings_.setValue("cleanupInProgress",true); settings_.sync();
        if(settings_.status()!=QSettings::NoError) { QMessageBox::warning(this,text("File operation failed"),text("The recovery review could not be saved. No files were moved.")); return; }
    }
    const std::optional<TransferRecord> record=undo?std::optional<TransferRecord>(*undo):std::nullopt;
    syncing_=false; linkedStepFromRight_.reset(); restorePositions_.reset(); bulkCancel_.store(false);
    auto oldLeft=left_->detachPlayer(),oldRight=right_->detachPlayer();
    fileOperationPending_=true; centralWidget()->setEnabled(false); menuBar()->setEnabled(false);
    auto progress=new QProgressDialog(text(undo?"Undoing transfer…":"Copying and verifying…"),text("Cancel"),0,0,this);
    progress->setObjectName("transferProgress"); progress->setWindowModality(Qt::WindowModal); progress->setMinimumDuration(0); progress->setAutoClose(false); progress->show();
    connect(progress,&QProgressDialog::canceled,this,[this] { bulkCancel_.store(true); });
    struct Outcome { FileOperations::TransferResult operation; QString journalError; };
    auto watcher=new QFutureWatcher<Outcome>(this);
    connect(watcher,&QFutureWatcher<Outcome>::finished,this,[this,watcher,progress,file,record] {
        const auto outcome=watcher->result(); watcher->deleteLater(); progress->hide(); progress->deleteLater();
        fileOperationPending_=false; centralWidget()->setEnabled(true); menuBar()->setEnabled(true); cacheError_=outcome.journalError;
        if(outcome.journalError.isEmpty()) finishCleanupRecovery();
        const bool membershipChanged=(!record && outcome.operation.moved) || (record && outcome.operation.succeeded() && record->moved);
        if(!record && outcome.operation.moved) for(auto& value:files_) if(value.fullPath==file.fullPath && value.identity==file.identity) value.deleted=true;
        if(record && outcome.operation.succeeded() && record->moved) {
            const auto restored=FileOperations::ReadIdentity(file.fullPath);
            for(auto& value:files_) if(value.fullPath==file.fullPath && restored.valid) { value.identity=restored; value.deleted=false; }
        }
        if(membershipChanged) refreshTransferResults(); else if(currentGroup_>=0) selectGroup(currentGroup_);
        updateUndoAction();
        const auto success=record?text("Transfer undone."):text("Transfer completed.");
        statusBar()->showMessage(outcome.operation.succeeded() && cacheError_.isEmpty()?success:text("The transfer needs attention. See operation history."));
        if((!outcome.operation.succeeded() || !cacheError_.isEmpty()) && !pendingClose_) {
            QString message=outcome.operation.succeeded()?QString():readError(outcome.operation.error);
            if(outcome.operation.copied && !outcome.operation.succeeded()) message+="\n"+text("A copy was created. Inspect both paths before retrying.");
            if(!cacheError_.isEmpty()) message+="\n"+cacheError_;
            auto warning=new QMessageBox(QMessageBox::Warning,text("File operation failed"),message,QMessageBox::Ok,this);
            warning->setAttribute(Qt::WA_DeleteOnClose); warning->setWindowModality(Qt::WindowModal); warning->open();
        }
        if(pendingClose_) close();
    });
    QPointer<QProgressDialog> guard(progress);
    watcher->setFuture(QtConcurrent::run([this,file,destination,operation,record,oldLeft,oldRight,guard] {
        Outcome outcome; outcome.operation.error=E_FAIL;
        Platform::ComInit com(Platform::ComApartment::ApartmentThreaded);
        try {
            oldLeft->Close(); oldRight->Close(); previewPool_.waitForDone();
            ScanCache journal(cachePath_);
            const auto id=record?record->id:journal.beginTransfer(file,destination,operation);
            const bool ready=id>0 && (!record || journal.beginTransferUndo(id));
            if(ready && com.succeeded()) {
                if(record) {
                    const auto kind=record->moved && !record->restoredIdentity.valid?FileOperations::TransferKind::Move:FileOperations::TransferKind::Copy;
                    outcome.operation=FileOperations::UndoTransfer(file.fullPath,destination.toStdWString(),kind,record->restoredIdentity.valid?record->restoredIdentity:record->sourceIdentity,record->destinationIdentity,record->contentHash,bulkCancel_);
                } else {
                    QElapsedTimer throttle; throttle.start();
                    outcome.operation=FileOperations::Transfer(file.fullPath,destination.toStdWString(),operation=="copy"?FileOperations::TransferKind::Copy:FileOperations::TransferKind::Move,file.identity,bulkCancel_,[this,guard,&throttle](uint64_t done,uint64_t total) {
                        if(throttle.elapsed()<100 && done<total) return; throttle.restart();
                        const int percent=total?int(std::min(999.0,double(done)/double(total)*1000)):0;
                        QMetaObject::invokeMethod(this,[guard,percent] { if(guard) { guard->setRange(0,1000); guard->setValue(percent); } },Qt::QueuedConnection);
                    });
                }
            } else if(!com.succeeded()) outcome.operation.error=com.hr();
            if(ready) {
                if(record) journal.completeTransferUndo(id,outcome.operation); else journal.completeTransfer(id,outcome.operation);
            }
            outcome.journalError=journal.error();
            if(!ready && outcome.journalError.isEmpty()) outcome.journalError=text("The transfer journal could not be updated. No automatic retry is allowed.");
        } catch(...) { outcome.journalError=text("The transfer outcome is unconfirmed. Inspect operation history and both paths."); }
        return outcome;
    }));
}
void MainWindow::refreshTransferResults() {
    std::vector<int> candidates;
    for(int i=0;i<int(files_.size());++i) if(!files_[i].deleted && !files_[i].corrupted) candidates.push_back(i);
    const int previousGroup=currentGroup_;
    std::vector<std::pair<int,int>> pairs;
    for(const auto& group:groups_) if(group.fileIndices.size()>=2) pairs.emplace_back(group.fileIndices.front(),group.fileIndices[group.rightSlot]);
    if(manual_) { groups_.clear(); if(candidates.size()>=2) { VideoGroup group; group.fileIndices=std::move(candidates); group.manualReference=true; groups_.push_back(std::move(group)); } }
    else { std::atomic<bool> cancel{false}; VideoScanner::GroupKnownFiles(files_,std::move(candidates),scanCriteria_,groups_,cancel); }
    for(auto& group:groups_) for(const auto& [left,right]:pairs) if(group.fileIndices.front()==left) {
        const auto found=std::find(group.fileIndices.begin()+1,group.fileIndices.end(),right);
        if(found!=group.fileIndices.end()) group.rightSlot=int(found-group.fileIndices.begin());
    }
    for(auto it=markedFiles_.begin();it!=markedFiles_.end();) { if(files_[*it].deleted) it=markedFiles_.erase(it); else ++it; }
    currentGroup_=-1; populateResults();
    if(!groups_.empty()) selectGroup(std::clamp(previousGroup,0,int(groups_.size())-1));
    else { left_->setFile(nullptr,-1); right_->setFile(nullptr,-1); right_->setCandidates(nullptr); groupLabel_->setText(text("No active group")); updateSimilarity(); }
}
void MainWindow::deleteFile(PlayerPanel* panel) {
    if(scanning_ || sortingMetadata_ || clearingCache_ || closing_ || fileOperationPending_ || panel->fileIndex<0 || currentGroup_<0) return;
    int index=panel->fileIndex; auto& file=files_[index]; if(file.protectedFile || protectedByReview(file)) return;
    auto originalIdentity=file.identity;
    if(!originalIdentity.valid || FileOperations::ReadIdentity(file.fullPath)!=originalIdentity) {
        QMessageBox::warning(this,text("File operation failed"),text("This file changed or is no longer available. Rescan before removing it.")); return;
    }
    // Like the bulk cleanup, a single removal needs another copy of the group that is still on disk unchanged,
    // so an unplugged drive or a copy deleted elsewhere never turns this into removing the last one.
    const auto& members=groups_[currentGroup_].fileIndices;
    const bool otherCopy=std::any_of(members.begin(),members.end(),[&](int other) {
        return other!=index && !files_[other].deleted && files_[other].identity.valid && files_[other].identity!=originalIdentity
            && FileOperations::ReadIdentity(files_[other].fullPath)==files_[other].identity;
    });
    if(!otherCopy) {
        QMessageBox::warning(this,text("File operation failed"),text("No other copy in this group is still available unchanged, so this file was kept. Rescan before removing it.")); return;
    }
    // The recovery review is captured here but written on the worker, before anything is moved.
    auto recoveryJob=sessionSaveJob(cleanupReviewPath_);
    if(!recoveryJob) { QMessageBox::warning(this,text("File operation failed"),text("The recovery review could not be saved. No files were moved.")); return; }
    int previousLeftIndex=groups_[currentGroup_].fileIndices[0];
    int previousRightSlot=groups_[currentGroup_].rightSlot;
    int previousRightIndex=(previousRightSlot>=1 && previousRightSlot<static_cast<int>(groups_[currentGroup_].fileIndices.size()))?groups_[currentGroup_].fileIndices[previousRightSlot]:-1;
    auto clock=std::make_shared<QElapsedTimer>(); clock->start();
    syncing_=false; left_->player->Pause(); right_->player->Pause();
    auto oldPlayer=panel->detachPlayer(); auto target=file;
    fileOperationPending_=true; centralWidget()->setEnabled(false); menuBar()->setEnabled(false);
    struct TrackedRemoval { FileOperations::Result operation; QString journalError; bool recoveryFailed=false; };
    auto watcher=new QFutureWatcher<TrackedRemoval>(this);
    connect(watcher,&QFutureWatcher<FileOperations::Result>::finished,this,[this,watcher,index,previousLeftIndex,previousRightSlot,previousRightIndex,clock] {
        trace("remove: worker finished",*clock);
        auto outcome=watcher->result(); auto result=outcome.operation; watcher->deleteLater(); fileOperationPending_=false;
        centralWidget()->setEnabled(true); menuBar()->setEnabled(true); cacheError_=outcome.journalError;
        finishCleanupRecovery();
        if(!result.succeeded()) {
            if(!pendingClose_) QMessageBox::warning(this,text("File operation failed"),outcome.recoveryFailed?text("The recovery review could not be saved. No files were moved.")
                : QString::fromStdWString(result.message())+(cacheError_.isEmpty()?QString():"\n"+cacheError_));
            selectGroup(currentGroup_); if(pendingClose_) close(); return;
        }
        auto& file=files_[index]; file.deleted=true; markedFiles_.remove(index); QString removed=QString::fromStdWString(file.fileName);
        if(!cacheError_.isEmpty() && !pendingClose_) QMessageBox::warning(this,text("Operation history"),cacheError_);
        if(scanCriteria_.crossFolderOnly && !manual_) {
            std::vector<int> candidates; for(int i=0;i<int(files_.size());++i) if(!files_[i].deleted && !files_[i].corrupted) candidates.push_back(i);
            std::atomic<bool> cancel{false}; VideoScanner::GroupKnownFiles(files_,std::move(candidates),scanCriteria_,groups_,cancel);
            populateResults(); currentGroup_=-1;
            if(!groups_.empty()) {
                int target=0; for(int g=0;g<int(groups_.size());++g) if(groups_[g].fileIndices.front()==previousLeftIndex) { target=g; break; }
                selectGroup(target);
            } else { left_->setFile(nullptr,-1); right_->setFile(nullptr,-1); right_->setCandidates(nullptr); groupLabel_->setText(text("No active group")); }
            statusBar()->showMessage(text("Moved to Recycle Bin: %1").arg(removed)); if(pendingClose_) close(); return;
        }
        int groupIndex=currentGroup_; auto remaining=groups_[groupIndex].fileIndices;
        std::erase(remaining,index);
        const int groupsBefore=int(groups_.size())-1;
        groups_.erase(groups_.begin()+groupIndex);
        if(remaining.size()>=2) {
            std::vector<VideoFileInfo> subset; for(int member:remaining) subset.push_back(files_[member]);
            std::vector<VideoGroup> replacement; std::atomic<bool> cancel{false}; auto criteria=scanCriteria_; criteria.loadPreviews=false;
            { ScanCache cache(cachePath_); criteria.ignoredPairs=cache.ignoredPairs(); }
            if(manual_) {
                VideoGroup g; for(int i=0;i<static_cast<int>(remaining.size());++i) g.fileIndices.push_back(i);
                replacement.push_back(std::move(g));
            } else {
                VideoScanner::AnalyzeAndGroup(subset,criteria,replacement,cancel,{}, {},nullptr,[&](auto& file) { StillImages::analyze(file,cancel); });
            }
            for(auto& group:replacement) {
                for(auto& member:group.fileIndices) member=remaining[member];
                if(index!=previousLeftIndex) {
                    auto it=std::find(group.fileIndices.begin(),group.fileIndices.end(),previousLeftIndex);
                    if(it!=group.fileIndices.end() && it!=group.fileIndices.begin()) std::rotate(group.fileIndices.begin(),it,it+1);
                    group.rightSlot=std::clamp(previousRightSlot,1,static_cast<int>(group.fileIndices.size())-1);
                } else {
                    int nextRight=1;
                    if(previousRightIndex>=0) {
                        for(int s=1;s<static_cast<int>(group.fileIndices.size());++s) if(group.fileIndices[s]==previousRightIndex) { nextRight=s; break; }
                    }
                    group.rightSlot=std::clamp(nextRight,1,static_cast<int>(group.fileIndices.size())-1);
                }
            }
            groups_.insert(groups_.begin()+groupIndex,replacement.begin(),replacement.end());
        }
        trace("remove: regrouped",*clock);
        replaceGroupRows(groupIndex,int(groups_.size())-groupsBefore);
        if(!groups_.empty()) {
            int nextGroup=std::min(groupIndex,static_cast<int>(groups_.size())-1);
            selectGroup(nextGroup,groups_[nextGroup].rightSlot); trace("remove: next pair selected",*clock);
        } else {
            currentGroup_=-1; left_->setFile(nullptr,-1); right_->setFile(nullptr,-1); right_->setCandidates(nullptr); groupLabel_->setText(text("No active group"));
        }
        statusBar()->showMessage(text("Moved to Recycle Bin: %1").arg(removed));
        if(pendingClose_) close();
    });
    watcher->setFuture(QtConcurrent::run([this,oldPlayer,target,originalIdentity,recoveryJob=std::move(recoveryJob),clock] {
        oldPlayer->Close(); trace("remove: player closed",*clock);
        previewPool_.waitForDone(); trace("remove: previews finished",*clock);
        bool recoveryReady=recoveryJob(); trace("remove: recovery review saved",*clock);
        // The UI thread only waits on this watcher through its event loop, so the blocking call cannot deadlock.
        if(recoveryReady) QMetaObject::invokeMethod(this,[this,&recoveryReady] { recoveryReady=markCleanupInProgress(); },Qt::BlockingQueuedConnection);
        if(!recoveryReady) { TrackedRemoval failed{{E_ABORT}}; failed.recoveryFailed=true; return failed; }
        Platform::ComInit com(Platform::ComApartment::ApartmentThreaded);
        ScanCache journal(cachePath_); auto id=journal.beginRemoval(target);
        FileOperations::Result result{!com.succeeded()?com.hr():E_FAIL};
        if(id>0 && com.succeeded()) result=FileOperations::Remove(nullptr,target.fullPath,true,originalIdentity);
        trace("remove: moved to trash",*clock);
        journal.completeRemoval(id,result.error);
        return TrackedRemoval{result,journal.error()};
    }));
}

int MainWindow::selectGroupKeeperSlot(const VideoGroup& group, const CleanupRetentionRule& rule) const {
    int bestSlot=-1;
    auto isProtected=[this](const VideoFileInfo& f) { return f.protectedFile || f.protectedByFolder || protectedByReview(f); };
    auto better=[&](int slotA, int slotB) {
        const auto& a=files_[group.fileIndices[slotA]];
        const auto& b=files_[group.fileIndices[slotB]];
        if(!rule.preferredFolder.empty()) {
            const bool inA=VideoScanner::IsWithinFolder(a.fullPath,rule.preferredFolder);
            const bool inB=VideoScanner::IsWithinFolder(b.fullPath,rule.preferredFolder);
            if(inA!=inB) return inA;
        }
        switch(rule.attribute) {
        case RetentionAttribute::CurrentSortOrder:
            return slotA<slotB;
        case RetentionAttribute::ResolutionHighest: {
            const uint64_t pixelsA=uint64_t(a.width)*uint64_t(a.height);
            const uint64_t pixelsB=uint64_t(b.width)*uint64_t(b.height);
            if(pixelsA!=pixelsB) return pixelsA>pixelsB;
            break;
        }
        case RetentionAttribute::DurationLongest:
            if(a.duration100ns!=b.duration100ns) return a.duration100ns>b.duration100ns;
            break;
        case RetentionAttribute::FpsHighest: {
            const int64_t fpsA=std::llround(a.fps*1000.0);
            const int64_t fpsB=std::llround(b.fps*1000.0);
            if(fpsA!=fpsB) return fpsA>fpsB;
            break;
        }
        case RetentionAttribute::SizeLargest:
            if(a.fileSize!=b.fileSize) return a.fileSize>b.fileSize;
            break;
        case RetentionAttribute::SizeSmallest:
            if(a.fileSize!=b.fileSize) return a.fileSize<b.fileSize;
            break;
        case RetentionAttribute::ModifiedNewest:
            if(a.identity.modified!=b.identity.modified) return a.identity.modified>b.identity.modified;
            break;
        case RetentionAttribute::ModifiedOldest:
            if(a.identity.modified!=b.identity.modified) return a.identity.modified<b.identity.modified;
            break;
        }
        const bool protA=isProtected(a), protB=isProtected(b);
        if(protA!=protB) return protA;
        return slotA<slotB;
    };
    for(int slot=0;slot<static_cast<int>(group.fileIndices.size());++slot) {
        const auto& file=files_[group.fileIndices[slot]];
        if(file.deleted || !file.identity.valid) continue;
        if(bestSlot<0 || better(slot,bestSlot)) bestSlot=slot;
    }
    return bestSlot;
}
std::vector<MainWindow::CleanupTarget> MainWindow::cleanupPlan(const CleanupRetentionRule& rule) const {
    std::set<std::pair<uint64_t,uint64_t>> keeperIds;
    std::vector<int> groupKeeperSlots(groups_.size(),-1);
    for(size_t g=0;g<groups_.size();++g) {
        const int keeperSlot=selectGroupKeeperSlot(groups_[g],rule);
        groupKeeperSlots[g]=keeperSlot;
        if(keeperSlot>=0) {
            const auto& keeper=files_[groups_[g].fileIndices[keeperSlot]];
            if(keeper.identity.valid) keeperIds.insert({keeper.identity.volume,keeper.identity.index});
        }
    }
    std::set<std::pair<uint64_t,uint64_t>> plannedIds;
    std::vector<CleanupTarget> plan;
    for(size_t g=0;g<groups_.size();++g) {
        const int keeperSlot=groupKeeperSlots[g];
        if(keeperSlot<0) continue;
        const auto& keeper=files_[groups_[g].fileIndices[keeperSlot]];
        for(int slot=0;slot<static_cast<int>(groups_[g].fileIndices.size());++slot) {
            if(slot==keeperSlot) continue;
            const int index=groups_[g].fileIndices[slot]; const auto& file=files_[index];
            if(file.deleted || file.protectedFile || file.protectedByFolder || protectedByReview(file) || !file.identity.valid) continue;
            const auto key=std::make_pair(file.identity.volume,file.identity.index);
            // A file retained by any group must never become another group's removal target.
            if(keeperIds.contains(key) || (!manual_ && !VideoScanner::FilesMatch(keeper,file,scanCriteria_)) || !plannedIds.insert(key).second) continue;
            plan.push_back({index,file,keeper});
        }
    }
    return plan;
}
void MainWindow::previewBulkCleanup() {
    if(scanning_ || sortingMetadata_ || clearingCache_ || closing_ || fileOperationPending_) return;
    CleanupRetentionRule activeRule;
    // Resolution, duration and FPS rules need metadata; it is read on a worker so slow or network drives cannot freeze the window.
    // Returns false when cancelled; the caller then keeps the previous rule.
    auto ensureGroupMetadata=[this](QWidget* parent) {
        std::vector<std::pair<int,VideoFileInfo>> missing; QSet<int> seen;
        for(const auto& group:groups_) for(int idx:group.fileIndices) {
            const auto& f=files_[idx];
            if(!f.deleted && !f.metadataLoaded && f.identity.valid && !seen.contains(idx)) { seen.insert(idx); missing.emplace_back(idx,f); }
        }
        if(missing.empty()) return true;
        QProgressDialog progress(text("Reading video information…"),text("Cancel"),0,int(missing.size()),parent);
        progress.setWindowModality(Qt::WindowModal); progress.setMinimumDuration(400); progress.setAutoClose(false); progress.setAutoReset(false);
        cancel_.store(false); sortingMetadata_=true;
        auto done=std::make_shared<std::atomic<int>>(0);
        QFutureWatcher<std::vector<std::pair<int,VideoFileInfo>>> watcher; QEventLoop loop; QTimer poll;
        connect(&watcher,&QFutureWatcherBase::finished,&loop,&QEventLoop::quit);
        connect(&progress,&QProgressDialog::canceled,this,[this] { cancel_.store(true); });
        connect(&poll,&QTimer::timeout,&progress,[&progress,done] { progress.setValue(done->load()); }); poll.start(100);
        watcher.setFuture(QtConcurrent::run([this,missing=std::move(missing),done]() mutable { readMetadata(missing,cancel_,done.get()); return missing; }));
        loop.exec(); poll.stop(); sortingMetadata_=false;
        auto result=watcher.result(); const bool complete=!cancel_.load();
        mergeMetadata(result);
        return complete;
    };
    auto plan=cleanupPlan(activeRule);
    QDialog dialog(this); dialog.setObjectName("bulkCleanupPlan"); dialog.setWindowTitle(text("Move other copies to Recycle Bin…")); dialog.resize(940,580);
    auto layout=new QVBoxLayout(&dialog);
    auto ruleRow=new QHBoxLayout; ruleRow->setSpacing(8);
    auto ruleLabel=new QLabel(text("Keep in each group"),&dialog); ruleRow->addWidget(ruleLabel);
    auto ruleCombo=new QComboBox(&dialog); ruleCombo->setObjectName("retentionRuleCombo"); ruleCombo->setAccessibleName(text("Keep in each group"));
    // Slot 0 is the reference, so "current order" keeps exactly the file the review showed on the left.
    ruleCombo->addItem(text("Reference file (shown on the left)"),int(RetentionAttribute::CurrentSortOrder));
    ruleCombo->addItem(text("Highest resolution"),int(RetentionAttribute::ResolutionHighest));
    if(!options_.images) {
        ruleCombo->addItem(text("Longest duration"),int(RetentionAttribute::DurationLongest));
        ruleCombo->addItem(text("Highest frame rate (FPS)"),int(RetentionAttribute::FpsHighest));
    }
    ruleCombo->addItem(text("Largest file size (not a quality guarantee)"),int(RetentionAttribute::SizeLargest));
    ruleCombo->addItem(text("Smallest file size"),int(RetentionAttribute::SizeSmallest));
    ruleCombo->addItem(text("Newest modified date"),int(RetentionAttribute::ModifiedNewest));
    ruleCombo->addItem(text("Oldest modified date"),int(RetentionAttribute::ModifiedOldest));
    ruleRow->addWidget(ruleCombo,1);
    auto folderLabel=new QLabel(text("Preferred folder"),&dialog); ruleRow->addWidget(folderLabel);
    auto folderCombo=new QComboBox(&dialog); folderCombo->setObjectName("preferredFolderCombo"); folderCombo->setAccessibleName(text("Preferred folder"));
    folderCombo->addItem(text("Any folder (no folder preference)"),QString());
    QStringList candidateFolders;
    for(const auto& root:folders_) {
        const auto norm=QDir::toNativeSeparators(QString::fromStdWString(root));
        if(!norm.isEmpty() && !candidateFolders.contains(norm,Qt::CaseInsensitive)) candidateFolders<<norm;
    }
    for(const auto& group:groups_) for(int idx:group.fileIndices) {
        const auto norm=QDir::toNativeSeparators(QString::fromStdWString(files_[idx].folderPath));
        if(!norm.isEmpty() && !candidateFolders.contains(norm,Qt::CaseInsensitive)) candidateFolders<<norm;
    }
    candidateFolders.sort(Qt::CaseInsensitive);
    for(const auto& f:candidateFolders) folderCombo->addItem(f,f);
    ruleRow->addWidget(folderCombo,1);
    layout->addLayout(ruleRow);
    auto summary=new QLabel(&dialog); summary->setObjectName("dialogLead"); summary->setWordWrap(true); layout->addWidget(summary);
    auto note=new QLabel(&dialog); note->setObjectName("cleanupSummaryNote"); note->setWordWrap(true); layout->addWidget(note);
    // Caveats stay available but no longer push the plan itself below the fold.
    auto detailsToggle=new QToolButton(&dialog); detailsToggle->setObjectName("cleanupRulesToggle"); detailsToggle->setCheckable(true); detailsToggle->setText(text("How this plan is chosen"));
    detailsToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon); detailsToggle->setArrowType(QGuiApplication::isRightToLeft()?Qt::LeftArrow:Qt::RightArrow); layout->addWidget(detailsToggle,0,Qt::AlignLeft);
    auto details=new QWidget(&dialog); auto detailsLayout=new QVBoxLayout(details); detailsLayout->setContentsMargins(18,0,0,4); details->hide(); layout->addWidget(details);
    auto tieNote=new QLabel(text("Priority: 1) Preferred folder (if selected), 2) Selected technical rule, 3) Protected status, 4) Reference file. Larger file size or resolution does not guarantee higher visual quality."),details);
    tieNote->setObjectName("retentionTieBreakerNote"); tieNote->setWordWrap(true); detailsLayout->addWidget(tieNote);
    auto evidence=new QLabel(text(scanCriteria_.matchContent&&!manual_?"Content verified":options_.images?"Related groups may contain indirect matches. Only direct matches to the retained file are included in this plan.":"Visual/metadata candidates are not proof of identical content. Review this plan before removing files."),details); evidence->setWordWrap(true); detailsLayout->addWidget(evidence);
    connect(detailsToggle,&QToolButton::toggled,&dialog,[details,detailsToggle](bool open) { details->setVisible(open); detailsToggle->setArrowType(open?Qt::DownArrow:QGuiApplication::isRightToLeft()?Qt::LeftArrow:Qt::RightArrow); });
    // The action is carried by icon and checkbox, so there is no per-row action column to read.
    auto tree=new QTreeWidget(&dialog); tree->setObjectName("cleanupPlanTree"); tree->setHeaderLabels({text("File"),text("Folder"),text("Size")});
    tree->setTextElideMode(Qt::ElideMiddle); tree->setRootIsDecorated(true); tree->setUniformRowHeights(true); tree->setIconSize(QSize(14,14));
    tree->header()->setSectionResizeMode(0,QHeaderView::Stretch); tree->header()->setSectionResizeMode(1,QHeaderView::Stretch); tree->header()->setSectionResizeMode(2,QHeaderView::ResizeToContents);
    tree->headerItem()->setTextAlignment(2,Qt::AlignRight|Qt::AlignVCenter); layout->addWidget(tree,1);
    auto buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&dialog);
    auto exportPlan=buttons->addButton(text("Export plan…"),QDialogButtonBox::ActionRole); exportPlan->setObjectName("exportCleanupPlan");
    auto selectedPlan=[&] {
        std::vector<CleanupTarget> chosen;
        for(int g=0;g<tree->topLevelItemCount();++g) for(int c=0;c<tree->topLevelItem(g)->childCount();++c) {
            auto row=tree->topLevelItem(g)->child(c); const int index=row->data(0,Qt::UserRole).toInt();
            if(row->checkState(0)==Qt::Checked && index>=0 && index<int(plan.size())) chosen.push_back(plan[index]);
        }
        return chosen;
    };
    connect(exportPlan,&QPushButton::clicked,&dialog,[&] {
        auto path=QFileDialog::getSaveFileName(&dialog,text("Export plan…"),{},"CSV (*.csv)");
        if(!path.isEmpty() && !exportCleanupPlan(path,selectedPlan())) QMessageBox::warning(&dialog,text("Export plan…"),text("The export could not be saved."));
    });
    // Destructive action: red and never the default button, so Enter cannot start it.
    auto confirm=buttons->button(QDialogButtonBox::Ok); confirm->setObjectName("dangerButton"); confirm->setAutoDefault(false); confirm->setDefault(false);
    confirm->style()->unpolish(confirm); confirm->style()->polish(confirm); // the button box already polished it under its old name
    auto cancel=buttons->button(QDialogButtonBox::Cancel); cancel->setText(text("Cancel")); cancel->setDefault(true);
    auto refreshCount=[&] {
        const auto chosen=selectedPlan(); uint64_t bytes=0; for(const auto& target:chosen) bytes+=target.file.fileSize;
        std::set<std::wstring> keepers; for(const auto& target:chosen) keepers.insert(target.keeper.fullPath);
        { const auto size=QString::fromStdWString(FormatBytesShort(bytes));
          summary->setText(keepers.size()==1?text("Remove %1 files · %2 · 1 group").arg(chosen.size()).arg(size):text("Remove %1 files · %2 · %3 groups").arg(chosen.size()).arg(size).arg(keepers.size())); }
        confirm->setText(chosen.size()==1?text("Move 1 file to Recycle Bin"):text("Move %1 files to Recycle Bin").arg(chosen.size())); confirm->setEnabled(!chosen.empty());
    };
    int appliedRule=ruleCombo->currentIndex();
    auto refreshDialogPlan=[&] {
        const auto attribute=static_cast<RetentionAttribute>(ruleCombo->currentData().toInt());
        if((attribute==RetentionAttribute::ResolutionHighest || attribute==RetentionAttribute::DurationLongest || attribute==RetentionAttribute::FpsHighest) && !ensureGroupMetadata(&dialog)) {
            if(pendingClose_) { dialog.reject(); return; }
            QSignalBlocker blocker(ruleCombo); ruleCombo->setCurrentIndex(appliedRule);
        }
        appliedRule=ruleCombo->currentIndex();
        activeRule.attribute=static_cast<RetentionAttribute>(ruleCombo->currentData().toInt());
        activeRule.preferredFolder=QDir::toNativeSeparators(folderCombo->currentData().toString()).toStdWString();
        plan=cleanupPlan(activeRule);
        // The heading already states the count and size; the note only adds what the heading cannot.
        note->setText(text("Covers all %1 result groups, including any hidden by the filter. Protected files are always kept. Cancelling stops the remaining moves; finished moves are not undone.").arg(groups_.size()));
        QSignalBlocker blocker(tree); tree->clear();
        auto folderOf=[](const VideoFileInfo& file) { return QDir::toNativeSeparators(file.folderPath.empty()?QFileInfo(QString::fromStdWString(file.fullPath)).absolutePath():QString::fromStdWString(file.folderPath)); };
        const QColor muted=Theme::color("@muted");
        // A keeper shared by several groups gets one "Keep" node, even when its groups are not adjacent.
        std::map<std::wstring,QTreeWidgetItem*> keeperNodes;
        for(int i=0;i<int(plan.size());++i) {
            const auto& target=plan[i];
            auto& group=keeperNodes[target.keeper.fullPath];
            if(!group) {
                group=new QTreeWidgetItem(tree,{QString::fromStdWString(target.keeper.fileName),folderOf(target.keeper),QString::fromStdWString(FormatBytesShort(target.keeper.fileSize))});
                auto font=group->font(0); font.setBold(true); group->setFont(0,font);
                group->setIcon(0,icon(Glyph::Check,"@keepIcon")); group->setForeground(1,muted); group->setTextAlignment(2,Qt::AlignRight|Qt::AlignVCenter);
                group->setToolTip(0,text("Keep")+": "+QString::fromStdWString(target.keeper.fullPath)); group->setToolTip(1,folderOf(target.keeper)); group->setFlags(Qt::ItemIsEnabled);
            }
            // Only a folder that differs from the kept file's folder is worth reading.
            const auto folder=folderOf(target.file); const bool sameFolder=folder.compare(folderOf(target.keeper),Qt::CaseInsensitive)==0;
            auto row=new QTreeWidgetItem(group,{QString::fromStdWString(target.file.fileName),sameFolder?QString():folder,QString::fromStdWString(FormatBytesShort(target.file.fileSize))});
            row->setData(0,Qt::UserRole,i); row->setFlags(Qt::ItemIsEnabled|Qt::ItemIsUserCheckable|Qt::ItemIsSelectable); row->setCheckState(0,Qt::Checked);
            row->setIcon(0,icon(Glyph::Trash,"@removeIcon")); row->setForeground(1,muted); row->setTextAlignment(2,Qt::AlignRight|Qt::AlignVCenter);
            row->setToolTip(0,text("Move to Recycle Bin")+": "+QString::fromStdWString(target.file.fullPath)); row->setToolTip(1,sameFolder?text("Same folder as the kept file"):folder);
        }
        tree->expandAll(); refreshCount();
    };
    refreshDialogPlan();
    connect(tree,&QTreeWidget::itemChanged,&dialog,[&] { refreshCount(); });
    connect(ruleCombo,&QComboBox::currentIndexChanged,&dialog,[&](int) { refreshDialogPlan(); });
    connect(folderCombo,&QComboBox::currentIndexChanged,&dialog,[&](int) { refreshDialogPlan(); });
    connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept); connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject); layout->addWidget(buttons);
    cancel->setFocus();
    if(dialog.exec()==QDialog::Accepted) executeBulkCleanup(selectedPlan());
    if(pendingClose_) close();
}

bool MainWindow::beginCleanupRecovery() {
    if(!saveSessionFile(cleanupReviewPath_) || !markCleanupInProgress()) {
        QMessageBox::warning(this,text("File operation failed"),text("The recovery review could not be saved. No files were moved.")); return false;
    }
    return true;
}
bool MainWindow::markCleanupInProgress() {
    recoverCleanupAction_->setEnabled(true);
    settings_.setValue("cleanupInProgress",true); settings_.sync();
    return settings_.status()==QSettings::NoError;
}
void MainWindow::finishCleanupRecovery() {
    settings_.setValue("cleanupInProgress",false); settings_.sync();
    if(settings_.status()!=QSettings::NoError) cacheError_+="\n"+text("The recovery state could not be updated. The saved review is still available.");
}
void MainWindow::executeBulkCleanup(std::vector<CleanupTarget> plan) {
    if(plan.empty() || fileOperationPending_ || scanning_ || sortingMetadata_ || closing_) return;
    std::erase_if(plan,[this](const auto& target) { return protectedByReview(target.file) || (!manual_ && !VideoScanner::FilesMatch(target.keeper,target.file,scanCriteria_)); });
    if(plan.empty()) return;
    if(!beginCleanupRecovery()) return;
    syncing_=false; linkedStepFromRight_.reset(); bulkCancel_.store(false);
    left_->player->Pause(); right_->player->Pause(); auto oldLeft=left_->detachPlayer(),oldRight=right_->detachPlayer();
    fileOperationPending_=true; centralWidget()->setEnabled(false); menuBar()->setEnabled(false);
    auto progress=new QProgressDialog(text("Moving files to Recycle Bin…"),text("Cancel"),0,int(plan.size()),this);
    progress->setObjectName("bulkCleanupProgress"); progress->setWindowModality(Qt::WindowModal); progress->setMinimumDuration(0); progress->setAutoClose(false); progress->show();
    connect(progress,&QProgressDialog::canceled,this,[this] { bulkCancel_.store(true); });
    struct BatchResult { std::vector<int> removed; QStringList errors; QString cacheError; int attempted=0; };
    auto watcher=new QFutureWatcher<BatchResult>(this);
    connect(watcher,&QFutureWatcher<BatchResult>::finished,this,[this,watcher,progress] {
        auto result=watcher->result(); watcher->deleteLater(); progress->hide(); progress->deleteLater();
        fileOperationPending_=false; centralWidget()->setEnabled(true); menuBar()->setEnabled(true);
        cacheError_=result.cacheError;
        finishCleanupRecovery(); result.cacheError=cacheError_;
        for(int index:result.removed) { files_[index].deleted=true; markedFiles_.remove(index); }
        for(auto& group:groups_) {
            std::erase_if(group.fileIndices,[this](int index) { return files_[index].deleted; });
            group.rightSlot=std::clamp(group.rightSlot,1,std::max(1,int(group.fileIndices.size())-1));
        }
        std::erase_if(groups_,[](const auto& group) { return group.fileIndices.size()<2; });
        currentGroup_=-1; left_->setFile(nullptr,-1); right_->setFile(nullptr,-1); right_->setCandidates(nullptr);
        populateResults(); if(!groups_.empty()) selectGroup(0); else groupLabel_->setText(text("No active group"));
        statusBar()->showMessage(text("Moved %1 files to Recycle Bin; %2 failed/skipped.").arg(result.removed.size()).arg(result.errors.size()));
        if((!result.errors.empty() || !result.cacheError.isEmpty()) && !pendingClose_) {
            // Do not enter a nested exec loop while the modal progress window is retiring.
            auto warning=new QMessageBox(QMessageBox::Warning,text("File operation failed"),result.errors.join("\n\n")+(result.cacheError.isEmpty()?QString():"\n"+result.cacheError),QMessageBox::Ok,this);
            warning->setAttribute(Qt::WA_DeleteOnClose); warning->setWindowModality(Qt::WindowModal); warning->open();
        }
        if(pendingClose_) close();
    });
    QPointer<QProgressDialog> progressGuard(progress);
    watcher->setFuture(QtConcurrent::run([this,plan=std::move(plan),oldLeft,oldRight,progressGuard,revalidate=savedResultEvidence_,criteria=scanCriteria_] {
        BatchResult result; oldLeft->Close(); oldRight->Close(); previewPool_.waitForDone();
        Platform::ComInit com(Platform::ComApartment::ApartmentThreaded);
        std::map<std::wstring,VideoFileInfo> verified;
        auto fresh=[&](const VideoFileInfo& source)->const VideoFileInfo& {
            auto [entry,inserted]=verified.try_emplace(source.fullPath);
            if(inserted) {
                auto& file=entry->second; file.fullPath=source.fullPath; file.fileName=source.fileName;
                file.identity=source.identity; file.fileSize=source.fileSize; file.comparisonRoot=source.comparisonRoot;
                if(FileOperations::ReadIdentity(file.fullPath)!=file.identity) file.error=HRESULT_FROM_WIN32(ERROR_FILE_INVALID);
                else if(criteria.matchContent) file.contentHash=FileOperations::ContentHash(file.fullPath,bulkCancel_,file.error);
                else if(criteria.images && (criteria.matchFirstFrame || criteria.matchResolution)) StillImages::analyze(file,bulkCancel_);
                else if(criteria.matchFirstFrame || criteria.matchVisualSamples) VideoScanner::ExtractMetadataAndFrame(file,criteria.matchVisualSamples,&bulkCancel_);
                else if(criteria.matchDuration || criteria.matchResolution) VideoScanner::ExtractMetadataOnly(file,&bulkCancel_);
                if(FileOperations::ReadIdentity(file.fullPath)!=file.identity) file.error=HRESULT_FROM_WIN32(ERROR_FILE_INVALID);
            }
            return entry->second;
        };
        { ScanCache cache(cachePath_);
          for(const auto& target:plan) {
            if(bulkCancel_.load()) break;
            auto intent=cache.beginRemoval(target.file);
            FileOperations::Result operation{intent>0?com.hr():E_FAIL};
            if(intent>0 && com.succeeded()) {
                if(!target.keeper.identity.valid || FileOperations::ReadIdentity(target.keeper.fullPath)!=target.keeper.identity)
                    operation.error=HRESULT_FROM_WIN32(ERROR_FILE_INVALID);
                else if(revalidate && !manual_ && [&] {
                    const auto& keeper=fresh(target.keeper); const auto& candidate=fresh(target.file);
                    return FAILED(keeper.error) || FAILED(candidate.error) || bulkCancel_.load() || !VideoScanner::FilesMatch(keeper,candidate,criteria);
                }()) operation.error=HRESULT_FROM_WIN32(ERROR_FILE_INVALID);
                else operation=FileOperations::Remove(nullptr,target.file.fullPath,true,target.file.identity);
            }
            ++result.attempted; cache.completeRemoval(intent,operation.error);
            if(operation.succeeded()) result.removed.push_back(target.index);
            else result.errors<<QString::fromStdWString(target.file.fullPath)+"\n"+QString::fromStdWString(operation.message());
            if(!cache.error().isEmpty()) break;
            QMetaObject::invokeMethod(this,[progressGuard,count=result.attempted] { if(progressGuard) progressGuard->setValue(count); },Qt::QueuedConnection);
          }
          result.cacheError=cache.error(); }
        return result;
    }));
}
bool MainWindow::exportCleanupPlan(const QString& path,const std::vector<CleanupTarget>& plan) {
    QSaveFile output(path); if(!output.open(QIODevice::WriteOnly)) return false;
    auto bytes=QByteArray::fromHex("efbbbf")+"keep_path,recycle_path,bytes,evidence,operation\r\n";
    if(output.write(bytes)!=bytes.size()) return false;
    for(const auto& target:plan) {
        QStringList row{QString::fromStdWString(target.keeper.fullPath),QString::fromStdWString(target.file.fullPath),QString::number(target.file.fileSize),
            csvEvidenceTag(target.file,scanCriteria_,manual_),"planned_recycle_bin"};
        for(auto& value:row) value=Csv::field(value);
        bytes=row.join(',').toUtf8()+"\r\n"; if(output.write(bytes)!=bytes.size()) return false;
    }
    return output.commit();
}

void MainWindow::previewCleanupMarks() {
    if(scanning_ || fileOperationPending_ || clearingCache_ || groups_.empty()) return;
    QDialog dialog(this); dialog.setWindowTitle(text("Preview cleanup marks…")); dialog.resize(900,500);
    auto layout=new QVBoxLayout(&dialog); auto note=new QLabel(text("Keeps the file shown on the left in each group and all protected files. Review the proposed marks before applying them."),&dialog); note->setWordWrap(true); layout->addWidget(note);
    auto tree=new QTreeWidget(&dialog); tree->setColumnCount(3); tree->setHeaderLabels({text("File"),text("Size"),text("Match")}); tree->header()->setSectionResizeMode(0,QHeaderView::Stretch); tree->setTextElideMode(Qt::ElideMiddle); layout->addWidget(tree,1);
    // Size and status take the width their text needs; the path column gives way and its full text is in the tooltip.
    tree->header()->setStretchLastSection(false); tree->header()->setSectionResizeMode(1,QHeaderView::ResizeToContents); tree->header()->setSectionResizeMode(2,QHeaderView::ResizeToContents);
    // Cross-folder groups can share a file; one kept as a group's reference must not be proposed for removal in another group.
    QSet<int> references; for(const auto& group:groups_) if(!group.fileIndices.empty()) references.insert(group.fileIndices[0]);
    for(const auto& group:groups_) {
        auto heading=new QTreeWidgetItem(tree,{QString::fromStdWString(files_[group.fileIndices[0]].fullPath),{},text("Keep (left file)")}); heading->setExpanded(true); heading->setToolTip(0,heading->text(0));
        for(size_t slot=1;slot<group.fileIndices.size();++slot) {
            int index=group.fileIndices[slot]; const auto& file=files_[index];
            bool protectedFile=file.protectedFile || protectedByReview(file); const bool keptElsewhere=references.contains(index);
            auto row=new QTreeWidgetItem(heading,{QString::fromStdWString(file.fullPath),QString::fromStdWString(FormatBytesShort(file.fileSize)),
                protectedFile?text("Protected"):keptElsewhere?text("Kept as another group's reference"):text(scanCriteria_.matchContent&&!manual_?"Content verified":"Review candidate")});
            row->setToolTip(0,row->text(0)); row->setToolTip(2,row->text(2));
            row->setData(0,Qt::UserRole,index); row->setCheckState(0,protectedFile || keptElsewhere?Qt::Unchecked:Qt::Checked);
            if(protectedFile || keptElsewhere) row->setDisabled(true);
        }
    }
    // An accept button, so each platform puts it in its usual place next to Cancel.
    auto buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(text("Apply marks")); buttons->button(QDialogButtonBox::Cancel)->setText(text("Cancel"));
    connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept); connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject); layout->addWidget(buttons);
    if(dialog.exec()!=QDialog::Accepted) return;
    markedFiles_.clear();
    for(int g=0;g<tree->topLevelItemCount();++g) for(int slot=0;slot<tree->topLevelItem(g)->childCount();++slot) {
        auto row=tree->topLevelItem(g)->child(slot); int index=row->data(0,Qt::UserRole).toInt();
        if(row->checkState(0)==Qt::Checked && !files_[index].protectedFile && !references.contains(index)) markedFiles_.insert(index);
    }
    populateResults();
    if(!markedFiles_.isEmpty()) statusBar()->showMessage(text("%1 files marked. Use Move marked files to Recycle Bin… to remove them.").arg(markedFiles_.size()));
}

std::vector<MainWindow::CleanupTarget> MainWindow::markedCleanupPlan() const {
    std::set<std::pair<uint64_t,uint64_t>> markedIds,plannedIds;
    for(int index:markedFiles_) if(files_[index].identity.valid) markedIds.insert({files_[index].identity.volume,files_[index].identity.index});
    std::vector<CleanupTarget> plan;
    for(const auto& group:groups_) for(int index:group.fileIndices) {
        if(!markedFiles_.contains(index)) continue;
        const auto& file=files_[index];
        if(file.deleted || file.protectedFile || file.protectedByFolder || protectedByReview(file) || !file.identity.valid) continue;
        // The keeper must stay unmarked and match the target directly; a fully marked group keeps everything.
        const VideoFileInfo* keeper=nullptr;
        for(int other:group.fileIndices) {
            const auto& candidate=files_[other];
            if(other==index || candidate.deleted || !candidate.identity.valid || markedIds.contains({candidate.identity.volume,candidate.identity.index})) continue;
            if(manual_ || VideoScanner::FilesMatch(candidate,file,scanCriteria_)) { keeper=&candidate; break; }
        }
        if(keeper && plannedIds.insert({file.identity.volume,file.identity.index}).second) plan.push_back({index,file,*keeper});
    }
    return plan;
}

void MainWindow::removeMarkedFiles() {
    if(scanning_ || sortingMetadata_ || clearingCache_ || closing_ || fileOperationPending_) return;
    auto plan=markedCleanupPlan();
    if(plan.empty()) {
        statusBar()->showMessage(text(markedFiles_.isEmpty()?"No files are marked for cleanup.":"Marked files have no unmarked matching copy to keep, or are protected."));
        return;
    }
    uint64_t bytes=0; QStringList lines;
    for(const auto& target:plan) { bytes+=target.file.fileSize; lines<<QString::fromStdWString(target.file.fullPath)+"\n    "+text("Keep")+": "+QString::fromStdWString(target.keeper.fullPath); }
    QMessageBox box(QMessageBox::Warning,text("Move marked files to Recycle Bin…"),
        text("Move %1 marked files (%2) to the Recycle Bin? Each one keeps an unmarked matching copy. Files are verified again before moving.").arg(plan.size()).arg(QString::fromStdWString(FormatBytesShort(bytes))),
        QMessageBox::NoButton,this);
    box.setDetailedText(lines.join('\n'));
    auto confirm=box.addButton(plan.size()==1?text("Move 1 file to Recycle Bin"):text("Move %1 files to Recycle Bin").arg(plan.size()),QMessageBox::DestructiveRole);
    confirm->setObjectName("dangerButton"); confirm->setAutoDefault(false);
    auto cancel=box.addButton(text("Cancel"),QMessageBox::RejectRole); box.setDefaultButton(cancel);
    box.exec();
    if(box.clickedButton()==confirm) executeBulkCleanup(std::move(plan));
}
