#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "Language.h"
#include "Icons.h"
#include "Theme.h"
#include "FileOperations.h"
#include "platform/Platform.h"
#include "ScanCache.h"
#include "ImageReader.h"
#include <QComboBox>
#include <QDir>
#include <QFutureWatcher>
#include <QLabel>
#include <QLayout>
#include <QMenuBar>
#include <QMessageBox>
#include <QPixmapCache>
#include <QPointer>
#include <QProgressBar>
#include <QProgressDialog>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QToolButton>
#include <QtConcurrent/QtConcurrentRun>

using Language::text;

void MainWindow::fillReferenceRules() {
    if(!referenceRule_) return;
    const auto current=referenceRule_->currentData(); QSignalBlocker blocker(referenceRule_); referenceRule_->clear();
    referenceRule_->addItem(text("Highest resolution"),int(ReferenceRule::HighestResolution));
    referenceRule_->addItem(text("Largest file"),int(ReferenceRule::LargestFile));
    if(!options_.images) referenceRule_->addItem(text("Longest duration"),int(ReferenceRule::LongestDuration));
    referenceRule_->addItem(text("Oldest file"),int(ReferenceRule::OldestFile));
    referenceRule_->addItem(text("Newest file"),int(ReferenceRule::NewestFile));
    referenceRule_->addItem(text("Oldest creation date"),int(ReferenceRule::OldestCreated));
    if(options_.images) referenceRule_->addItem(text("Oldest date taken (EXIF)"),int(ReferenceRule::OldestCaptureDate));
    referenceRule_->addItem(text("First by name"),int(ReferenceRule::NameOrder));
    const int found=current.isValid()?referenceRule_->findData(current):-1; referenceRule_->setCurrentIndex(found>=0?found:0);
    referenceRule_->setAccessibleName(text("Reference file in each group"));
    referenceRule_->setToolTip(text("The file shown on the left and kept by default. Protected files always come first. Right-click a thumbnail to change it for one group."));
}
// Orders every group so slot 0 is the reference: protected files first, then the chosen rule, then the larger file.
void MainWindow::applyReferenceRule() {
    if(!referenceRule_) return;
    const auto rule=static_cast<ReferenceRule>(referenceRule_->currentData().toInt());
    settings_.setValue("referenceRule",int(rule));
    if(scanning_ || sortingMetadata_ || fileOperationPending_ || closing_ || groups_.empty()) return;
    // Identical content has identical resolution, duration and EXIF bytes, so only mixed groups need their metadata read first.
    if(rule==ReferenceRule::HighestResolution || rule==ReferenceRule::LongestDuration || rule==ReferenceRule::OldestCaptureDate)
        for(const auto& group:groups_) if(!group.manualReference && proofLevel(group)!=2)
            for(int i:group.fileIndices) if((!files_[i].metadataLoaded || (rule==ReferenceRule::OldestCaptureDate && StillImages::supported(files_[i].fullPath) && files_[i].cachedDetailsVersion<2)) && !files_[i].corrupted && !files_[i].previewFailed && !files_[i].deleted) { leftOnlySorting_=true; sortResults(); return; }
    if(rule==ReferenceRule::OldestCreated || rule==ReferenceRule::OldestCaptureDate)
        for(const auto& group:groups_) if(!group.manualReference)
            for(int i:group.fileIndices) if(files_[i].createdTime==0 && files_[i].identity.valid && !files_[i].deleted)
                files_[i].createdTime=Platform::ReadFileCreationTime(files_[i].fullPath);
    auto before=[&](int a,int b) {
        const auto& x=files_[a]; const auto& y=files_[b];
        if(x.protectedFile!=y.protectedFile) return x.protectedFile;
        switch(rule) {
        case ReferenceRule::HighestResolution: if(x.PixelCount()!=y.PixelCount()) return x.PixelCount()>y.PixelCount(); break;
        case ReferenceRule::LongestDuration: if(x.duration100ns!=y.duration100ns) return x.duration100ns>y.duration100ns; break;
        case ReferenceRule::OldestFile: if(x.identity.modified!=y.identity.modified) return x.identity.modified<y.identity.modified; break;
        case ReferenceRule::NewestFile: if(x.identity.modified!=y.identity.modified) return x.identity.modified>y.identity.modified; break;
        case ReferenceRule::OldestCreated: {
            const uint64_t ca=x.createdTime?x.createdTime:x.identity.modified;
            const uint64_t cb=y.createdTime?y.createdTime:y.identity.modified;
            if(ca!=cb) return ca<cb;
            break;
        }
        case ReferenceRule::OldestCaptureDate: {
            if((x.captureTimeSec>0)!=(y.captureTimeSec>0)) return x.captureTimeSec>0;
            if(x.captureTimeSec>0 && x.captureTimeSec!=y.captureTimeSec) return x.captureTimeSec<y.captureTimeSec;
            const uint64_t ca=x.createdTime?x.createdTime:x.identity.modified;
            const uint64_t cb=y.createdTime?y.createdTime:y.identity.modified;
            if(ca!=cb) return ca<cb;
            break;
        }
        case ReferenceRule::NameOrder: if(const int order=QString::fromStdWString(x.fileName).localeAwareCompare(QString::fromStdWString(y.fileName))) return order<0; break;
        case ReferenceRule::LargestFile: break;
        }
        if(x.fileSize!=y.fileSize) return x.fileSize>y.fileSize;
        return x.fullPath<y.fullPath;
    };
    const auto scroll=results_->verticalScrollBar()->value();
    const auto activeMembers=(currentGroup_>=0 && currentGroup_<int(groups_.size()))?groups_[currentGroup_].fileIndices:std::vector<int>{};
    int active=currentGroup_;
    for(auto& group:groups_) {
        if(!group.manualReference) {
            const int right=group.fileIndices.size()>size_t(group.rightSlot)?group.fileIndices[group.rightSlot]:-1;
            std::stable_sort(group.fileIndices.begin(),group.fileIndices.end(),before);
            // The rule's favourite becomes the reference only if it can anchor the group; otherwise the best eligible file does.
            if(auto eligible=std::find_if(group.fileIndices.begin(),group.fileIndices.end(),[&](int member) { return canBeReference(group,member) && matchesWholeGroup(group,member); }); eligible!=group.fileIndices.end())
                std::rotate(group.fileIndices.begin(),eligible,eligible+1);
            group.leftSlot=0; auto found=std::find(group.fileIndices.begin()+std::min<size_t>(1,group.fileIndices.size()),group.fileIndices.end(),right);
            group.rightSlot=found!=group.fileIndices.end()?int(found-group.fileIndices.begin()):1;
        }
        group.bestFrameSim=group.fileIndices.size()>=2?VideoScanner::ComputePairSimilarity(files_[group.fileIndices[0]],files_[group.fileIndices[group.rightSlot]],manual_?options_.matchTransforms:scanCriteria_.matchTransforms):0.0;
    }
    if(options_.sortOrder==SortOrder::SimilarityDesc || options_.sortOrder==SortOrder::SimilarityAsc) {
        const bool desc=options_.sortOrder==SortOrder::SimilarityDesc;
        auto simRank=[](double s)->int64_t { return std::isfinite(s)?std::llround(std::clamp(s,0.0,100.0)*10000.0):-1; };
        std::stable_sort(groups_.begin(),groups_.end(),[&](const VideoGroup& ga,const VideoGroup& gb) {
            if(ga.fileIndices.empty() || gb.fileIndices.empty()) return ga.fileIndices.size()>gb.fileIndices.size();
            const bool exactA=proofLevel(ga)==2, exactB=proofLevel(gb)==2;
            if(exactA!=exactB) return desc?exactA:!exactA;
            const int64_t rankA=simRank(ga.bestFrameSim), rankB=simRank(gb.bestFrameSim);
            if(rankA!=rankB) return desc?(rankA>rankB):(rankA<rankB);
            const auto& a=files_[ga.fileIndices[0]]; const auto& b=files_[gb.fileIndices[0]];
            if(a.protectedFile!=b.protectedFile) return a.protectedFile;
            if(a.fileSize!=b.fileSize) return a.fileSize>b.fileSize;
            return a.fullPath<b.fullPath;
        });
        if(!activeMembers.empty()) {
            auto sameSet=[](std::vector<int> u,std::vector<int> v) { std::sort(u.begin(),u.end()); std::sort(v.begin(),v.end()); return u==v; };
            for(size_t g=0;g<groups_.size();++g) if(sameSet(groups_[g].fileIndices,activeMembers)) { active=int(g); break; }
        }
    }
    populateResults(); if(active>=0 && active<int(groups_.size())) selectGroup(active,groups_[active].rightSlot);
    results_->verticalScrollBar()->setValue(scroll);
}
// Cross-folder groups are built around an anchor whose folder no other member shares; only such a file can be the reference.
bool MainWindow::canBeReference(const VideoGroup& group,int member) const {
    if(manual_ || !scanCriteria_.crossFolderOnly) return true;
    return std::none_of(group.fileIndices.begin(),group.fileIndices.end(),[&](int other) { return other!=member && files_[other].comparisonRoot==files_[member].comparisonRoot; });
}
// Similar-video groups are built around one file that matches every member; similarity is not transitive,
// so a reference that misses a member would compare that member against a file it never matched.
bool MainWindow::matchesWholeGroup(const VideoGroup& group,int member) const {
    if(manual_ || options_.images) return true;
    return std::all_of(group.fileIndices.begin(),group.fileIndices.end(),[&](int other) { return other==member || VideoScanner::FilesMatch(files_[member],files_[other],scanCriteria_); });
}
// One group's reference chosen by hand: the previous reference becomes the compared candidate.
void MainWindow::makeReference(int groupIndex,int slot) {
    if(scanning_ || sortingMetadata_ || fileOperationPending_ || closing_ || groupIndex<0 || groupIndex>=int(groups_.size())) return;
    auto& group=groups_[groupIndex];
    if(slot<=0 || slot>=int(group.fileIndices.size())) return;
    if(!canBeReference(group,group.fileIndices[slot])) { statusBar()->showMessage(text("Cross-folder results compare against another folder, so the reference must be the only file from its folder in this group.")); return; }
    std::rotate(group.fileIndices.begin(),group.fileIndices.begin()+slot,group.fileIndices.begin()+slot+1);
    group.manualReference=true; group.leftSlot=0; group.rightSlot=1; reviewDirty_=true;
    group.bestFrameSim=group.fileIndices.size()>=2?VideoScanner::ComputePairSimilarity(files_[group.fileIndices[0]],files_[group.fileIndices[group.rightSlot]],manual_?options_.matchTransforms:scanCriteria_.matchTransforms):0.0;
    updateGroupRow(groupIndex); selectGroup(groupIndex,1);
    statusBar()->showMessage(text("Reference set for this group: %1").arg(QString::fromStdWString(files_[group.fileIndices[0]].fileName)));
}
void MainWindow::sortResults() {
    if(sortingMetadata_ || closing_ || scanning_ || fileOperationPending_) return;
    const bool needSim=!leftOnlySorting_ && (options_.sortOrder==SortOrder::SimilarityDesc || options_.sortOrder==SortOrder::SimilarityAsc);
    const bool needMeta=leftOnlySorting_ || (options_.sortOrder==SortOrder::DurationDesc || options_.sortOrder==SortOrder::DurationAsc || options_.sortOrder==SortOrder::ResolutionDesc || options_.sortOrder==SortOrder::ResolutionAsc);
    std::vector<std::pair<int,VideoFileInfo>> missing;
    if(needMeta || needSim) {
        const bool needCaptureDate=leftOnlySorting_ && referenceRule_ && referenceRule_->currentData().toInt()==int(ReferenceRule::OldestCaptureDate);
        QSet<int> seen;
        for(const auto& group:groups_) {
            if(needSim && !needMeta && proofLevel(group)==2) continue;
            for(int idx:group.fileIndices) {
                if(seen.contains(idx) || files_[idx].deleted || files_[idx].corrupted || files_[idx].previewFailed) continue;
                const bool lacksMeta=needMeta && (!files_[idx].metadataLoaded || (needCaptureDate && StillImages::supported(files_[idx].fullPath) && files_[idx].cachedDetailsVersion<2));
                const bool lacksSim=needSim && (options_.images
                    ? !files_[idx].frameLoaded
                    : (files_[idx].samples.size()!=5 && (!files_[idx].metadataLoaded || files_[idx].duration100ns>0) && !previewRuns_.value(idx,false)));
                if(lacksMeta || lacksSim) {
                    seen.insert(idx); missing.emplace_back(idx,files_[idx]);
                }
            }
        }
    }
    if(!missing.empty()) {
        cancel_.store(false);
        sortingMetadata_=true;
        centralWidget()->setEnabled(false); menuBar()->setEnabled(false);
        progress_->setRange(0,0); progress_->show(); statusBar()->showMessage(text(options_.images?"Reading image information for sorting…":"Reading video information for sorting…"));
        auto watcher=new QFutureWatcher<std::vector<std::pair<int,VideoFileInfo>>>(this);
        auto generation=generation_;
        connect(watcher,&QFutureWatcherBase::finished,this,[this,watcher,generation,needSim] {
            auto result=watcher->result(); watcher->deleteLater(); sortingMetadata_=false;
            if(pendingClose_) { close(); return; }
            if(closing_ || generation!=generation_) return;
            if(needSim) for(const auto& [idx,_]:result) previewRuns_.insert(idx,true);
            mergeMetadata(result);
            centralWidget()->setEnabled(true); menuBar()->setEnabled(true); progress_->hide();
            sortResults(); statusBar()->showMessage(text("Ready"));
        });
        watcher->setFuture(QtConcurrent::run(&previewPool_,[this,missing=std::move(missing),needSim]() mutable { readMetadata(missing,cancel_,nullptr,needSim); return missing; }));
        return;
    }
    if(leftOnlySorting_) { leftOnlySorting_=false; if(!cancel_.load()) applyReferenceRule(); return; }
    if(scanCriteria_.crossFolderOnly && !manual_) {
        QSet<int> chosen; for(const auto& group:groups_) if(group.manualReference && !group.fileIndices.empty()) chosen.insert(group.fileIndices[0]);
        std::vector<int> candidates;
        for(int i=0;i<int(files_.size());++i) if(!files_[i].deleted && !files_[i].corrupted && files_[i].fileSize>=scanCriteria_.minFileSize && files_[i].fileSize<=scanCriteria_.maxFileSize) candidates.push_back(i);
        std::atomic<bool> cancel{false}; VideoScanner::GroupKnownFiles(files_,std::move(candidates),scanCriteria_,groups_,cancel);
        for(auto& group:groups_) for(auto it=group.fileIndices.begin();it!=group.fileIndices.end();++it) if(chosen.contains(*it) && canBeReference(group,*it)) {
            std::rotate(group.fileIndices.begin(),it,it+1); group.manualReference=true; break;
        }
    }
    // Members are never dropped here: a group stays whole, and applyReferenceRule below picks a reference that matches all of it.
    VideoScanner::SortGroups(files_,groups_,options_.sortOrder,scanCriteria_.matchTransforms);
    for(auto& group:groups_) { group.leftSlot=0; group.rightSlot=1; }
    populateResults(); if(!groups_.empty()) selectGroup(0,1); applyReferenceRule(); saveSettings();
}
// Worker side of a metadata read: identity is checked before and after, so a changed file is never trusted.
void MainWindow::readMetadata(std::vector<std::pair<int,VideoFileInfo>>& missing,std::atomic<bool>& cancel,std::atomic<int>* done,bool sampleVideos) {
    Platform::ComInit com(Platform::ComApartment::MultiThreaded);
    std::vector<VideoFileInfo> cacheable;
    for(auto& [index,file]:missing) {
        if(cancel.load()) break;
        if(FileOperations::ReadIdentity(file.fullPath)!=file.identity) {
            file.contentHash.clear(); file.metadataLoaded=true; file.corrupted=true; file.previewFailed=false; file.error=HRESULT_FROM_WIN32(ERROR_FILE_INVALID);
        } else {
            if(file.createdTime==0) file.createdTime=Platform::ReadFileCreationTime(file.fullPath);
            if(StillImages::supported(file.fullPath)) {
                if((file.frameLoaded || (!sampleVideos && file.metadataLoaded)) && file.width>0 && file.height>0 && file.cachedDetailsVersion<2) StillImages::refreshCaptureTime(file,cancel);
                else StillImages::analyze(file,cancel);
            } else if(sampleVideos) { VideoScanner::ExtractMetadataOnly(file,&cancel); VideoScanner::ExtractMetadataAndFrame(file,true,&cancel); }
            else VideoScanner::ExtractMetadataOnly(file,&cancel);
            if(FileOperations::ReadIdentity(file.fullPath)!=file.identity) {
                file.contentHash.clear(); file.corrupted=true; file.previewFailed=false; file.error=HRESULT_FROM_WIN32(ERROR_FILE_INVALID);
            } else if((sampleVideos || file.metadataLoaded) && !cancel.load()) {
                cacheable.push_back(file);
            }
        }
        if(done) ++*done;
    }
    if(!cacheable.empty() && !cachePath_.isEmpty()) {
        ScanCache cache(cachePath_);
        cache.store(cacheable);
    }
}
void MainWindow::mergeMetadata(std::vector<std::pair<int,VideoFileInfo>>& result) {
    for(auto& [index,metadata]:result) {
        auto& file=files_[index];
        if(file.deleted || file.identity!=metadata.identity || file.fullPath!=metadata.fullPath) continue;
        // Previews may finish meanwhile; retain their pixels and signatures.
        file.duration100ns=metadata.duration100ns; file.width=metadata.width; file.height=metadata.height;
        if(metadata.createdTime>0) file.createdTime=metadata.createdTime;
        if(metadata.cachedDetailsVersion>=2 || metadata.captureTimeSec>0) file.captureTimeSec=metadata.captureTimeSec;
        file.videoBitrate=metadata.videoBitrate; file.audioChannels=metadata.audioChannels; file.audioSampleRate=metadata.audioSampleRate; file.audioCodec=metadata.audioCodec;
        file.fps=metadata.fps; file.codec=std::move(metadata.codec); file.metadataLoaded=metadata.metadataLoaded;
        if(metadata.metadataLoaded) { file.cachedDetailsVersion=metadata.cachedDetailsVersion; file.cachedImageRenderVersion=metadata.cachedImageRenderVersion; }
        if(metadata.frameLoaded && !file.frameLoaded) {
            file.sigFirst=metadata.sigFirst; file.sigAlt=metadata.sigAlt; file.frameLoaded=true;
            file.imageKind=metadata.imageKind; file.imageFrameCount=metadata.imageFrameCount;
            file.cachedDetailsVersion=metadata.cachedDetailsVersion; file.cachedImageRenderVersion=metadata.cachedImageRenderVersion;
            if(file.thumbBGRA.empty()) file.thumbBGRA=std::move(metadata.thumbBGRA);
        }
        if(metadata.samples.size()==5 && file.samples.size()!=5) file.samples=std::move(metadata.samples);
        if(metadata.corrupted) { file.contentHash.clear(); file.corrupted=true; file.previewFailed=false; file.error=metadata.error; }
        else if(metadata.previewFailed) { file.previewFailed=true; file.error=metadata.error; }
    }
}

int MainWindow::proofLevel(const VideoGroup& group) const {
    std::set<std::pair<uint64_t,std::string>> hashes;
    bool all=true,paired=false;
    for(int index:group.fileIndices) {
        const auto& f=files_[index];
        if(f.deleted || f.corrupted || !f.identity.valid || f.contentHash.empty()) { all=false; continue; }
        if(!hashes.insert({f.fileSize,f.contentHash}).second) paired=true;
    }
    return all && group.fileIndices.size()>=2 && hashes.size()==1?2:paired?1:0;
}
void MainWindow::sortVerifiedGroups() {
    if(scanning_ || sortingMetadata_ || fileOperationPending_ || clearingCache_ || closing_) return;
    const auto active=currentGroup_>=0?groups_[currentGroup_].fileIndices:std::vector<int>{};
    std::stable_partition(groups_.begin(),groups_.end(),[this](const auto& group) { return proofLevel(group)==2; });
    currentGroup_=-1;
    for(size_t i=0;i<groups_.size();++i) if(groups_[i].fileIndices==active) { currentGroup_=int(i); break; }
    populateResults(); showGroupPosition();
}
// The bar shows a compact "3 / 40"; the row itself carries the group number, the full wording goes to tooltips and screen readers.
void MainWindow::showGroupPosition(const QString& state) {
    if(currentGroup_<0 || currentGroup_>=int(groups_.size())) { groupLabel_->setText(text("No active group")); groupLabel_->setAccessibleName(groupLabel_->text()); groupLabel_->setToolTip({}); return; }
    const auto full=text("Group %1 of %2").arg(currentGroup_+1).arg(groups_.size())+(state.isEmpty()?QString():" · "+state);
    groupLabel_->setText(QString("%1 / %2").arg(currentGroup_+1).arg(groups_.size())+(state.isEmpty()?QString():"  ·  "+state));
    groupLabel_->setAccessibleName(full); groupLabel_->setToolTip(full);
}
void MainWindow::verifyResultHashes() {
    if(scanning_ || sortingMetadata_ || fileOperationPending_ || clearingCache_ || closing_ || groups_.empty()) return;
    std::set<int> members; for(const auto& group:groups_) members.insert(group.fileIndices.begin(),group.fileIndices.end());
    std::vector<VideoFileInfo> jobs; jobs.reserve(members.size());
    for(int index:members) {
        const auto& source=files_[index]; if(source.deleted || !source.identity.valid) continue;
        VideoFileInfo f; f.fullPath=source.fullPath; f.fileName=source.fileName; f.identity=source.identity; f.fileSize=source.fileSize; jobs.push_back(std::move(f));
    }
    cancel_.store(false); sortingMetadata_=true; syncing_=false; left_->player->Pause(); right_->player->Pause();
    const auto generation=generation_; const auto cachePath=cachePath_;
    auto progress=new QProgressDialog(text("Verifying same-size candidates with SHA-256…"),text("Cancel"),0,0,this);
    progress->setWindowModality(Qt::WindowModal); progress->setMinimumDuration(0); progress->show();
    connect(progress,&QProgressDialog::canceled,this,[this] { cancel_.store(true); });
    auto watcher=new QFutureWatcher<std::vector<VideoFileInfo>>(this); verificationWatcher_=watcher;
    connect(watcher,&QFutureWatcherBase::finished,this,[this,watcher,progress,generation] {
        auto result=watcher->result(); watcher->deleteLater(); verificationWatcher_=nullptr; progress->hide(); progress->deleteLater(); sortingMetadata_=false;
        if(pendingClose_) { close(); return; }
        if(closing_ || generation!=generation_ || cancel_.load()) return;
        unsigned checked=0;
        std::map<std::wstring,const VideoFileInfo*> byPath; for(const auto& verified:result) byPath.emplace(verified.fullPath,&verified);
        for(auto& f:files_) {
            const auto found=byPath.find(f.fullPath); if(found==byPath.end() || f.identity!=found->second->identity) continue;
            const auto& verified=*found->second;
            if(FAILED(verified.error)) f.contentHash.clear();
            else if(!verified.contentHash.empty()) { f.contentHash=verified.contentHash; ++checked; }
        }
        for(auto& g:groups_) if(g.fileIndices.size()>=2) {
            const int r=std::clamp(g.rightSlot,1,int(g.fileIndices.size())-1);
            g.bestFrameSim=VideoScanner::ComputePairSimilarity(files_[g.fileIndices[0]],files_[g.fileIndices[r]],manual_?options_.matchTransforms:scanCriteria_.matchTransforms);
        }
        populateResults(); updateSimilarity(); statusBar()->showMessage(text("%1 files SHA-checked; other results remain review candidates.").arg(checked));
    });
    watcher->setFuture(QtConcurrent::run([this,jobs=std::move(jobs),cachePath]() mutable {
        try {
            ScanCache cache(cachePath); for(auto& f:jobs) cache.restore(f);
            ScanCriteria criteria; criteria.matchContent=true; criteria.matchSize=true; criteria.matchDuration=false; criteria.loadPreviews=false; criteria.minFileSize=1;
            std::vector<VideoGroup> ignored; VideoScanner::AnalyzeAndGroup(jobs,criteria,ignored,cancel_);
            cache.store(jobs);
        } catch(...) { for(auto& f:jobs) { f.contentHash.clear(); f.error=E_FAIL; } }
        return jobs;
    }));
}
void MainWindow::updateSimilarity() {
    updatePeers();
    if(!partialVerdict_.isEmpty() && partialPair_==std::pair{left_->fileIndex,right_->fileIndex}) { setVerdict(partialVerdict_,partialVerdictKind_); return; }
    int left=left_->fileIndex,right=right_->fileIndex;
    if(left<0 || right<0 || left>=static_cast<int>(files_.size()) || right>=static_cast<int>(files_.size())) {
        setVerdict(text("No pair selected"),"none"); return;
    }
    const auto& a=files_[left]; const auto& b=files_[right];
    if(!a.contentHash.empty() && a.contentHash==b.contentHash && a.fileSize==b.fileSize) {
        setVerdict(text("Exact copy · 100% identical file contents"),"identical"); return;
    }
    if(!manual_ && profile_->currentIndex()==2 && !scanCriteria_.matchFirstFrame && !scanCriteria_.matchVisualSamples &&
       (scanCriteria_.matchFileName || scanCriteria_.matchExtension) && VideoScanner::FilesMatch(a,b,scanCriteria_)) {
        setVerdict(text("Filename candidate · content not verified"),"candidate"); return;
    }
    double score=0; unsigned valid=0;
    if(options_.images) {
        if(a.frameLoaded && b.frameLoaded) {
            if(a.sigFirst.lowVariance || b.sigFirst.lowVariance) { setVerdict(text("Visual similarity unavailable: insufficient image detail"),"none"); return; }
            auto criteria=activeCriteria(); criteria.matchContent=false; criteria.matchSize=false; criteria.matchResolution=false; criteria.matchFileName=false; criteria.matchExtension=false; criteria.crossFolderOnly=false; criteria.matchFirstFrame=true;
            criteria.matchTransforms=manual_?options_.matchTransforms:scanCriteria_.matchTransforms;
            score=PerceptualHash::CompareImageFiles(a,b,criteria.matchTransforms,criteria.minFrameSimilarity);
            if(score<=0) {
                if(!manual_ && profile_->currentIndex()==2 && (scanCriteria_.matchFileName || scanCriteria_.matchExtension) && VideoScanner::FilesMatch(a,b,scanCriteria_)) {
                    setVerdict(text("Filename candidate · content not verified"),"candidate"); return;
                }
                if((a.imageKind==ImageEvidenceKind::AnimatedImage)!=(b.imageKind==ImageEvidenceKind::AnimatedImage)) {
                    setVerdict(text("Visual similarity unavailable: static image and animation are not compared as equal"),"none"); return;
                }
                if((a.imageKind==ImageEvidenceKind::MultiPageDocument)!=(b.imageKind==ImageEvidenceKind::MultiPageDocument) ||
                   (a.imageKind==ImageEvidenceKind::MultiPageDocument && a.imageFrameCount!=b.imageFrameCount)) {
                    setVerdict(text("Visual similarity unavailable: multi-page count mismatch"),"none"); return;
                }
                if((a.imageKind==ImageEvidenceKind::RawEmbeddedPreview && b.imageKind==ImageEvidenceKind::RawDeveloped) ||
                   (a.imageKind==ImageEvidenceKind::RawDeveloped && b.imageKind==ImageEvidenceKind::RawEmbeddedPreview)) {
                    setVerdict(text("Visual similarity unavailable: RAW embedded preview vs developed RAW"),"none"); return;
                }
                setVerdict(text("Visual similarity unavailable"),"none"); return;
            }
            if(!manual_ && !VideoScanner::FilesMatch(a,b,scanCriteria_)) {
                setVerdict(text("Related group · this pair does not meet the scan criteria (%1%)").arg(score,0,'f',1),"candidate"); return;
            }
            const auto kind=(a.imageKind!=ImageEvidenceKind::StaticImage)?a.imageKind:b.imageKind;
            switch(kind) {
            case ImageEvidenceKind::AnimatedImage:
                setVerdict(text("Animated visual similarity: %1% · %2/%3 frames sampled (not a content identity check)").arg(score,0,'f',1).arg(a.samples.size()).arg(std::max(a.imageFrameCount,b.imageFrameCount)),"similar");
                break;
            case ImageEvidenceKind::MultiPageDocument:
                setVerdict(text("Multi-page visual similarity: %1% · %2 pages (not a content identity check)").arg(score,0,'f',1).arg(std::max(a.imageFrameCount,b.imageFrameCount)),"similar");
                break;
            case ImageEvidenceKind::MultiSizeIcon:
                setVerdict(text("Icon visual similarity: %1% · largest entry compared (not a content identity check)").arg(score,0,'f',1),"similar");
                break;
            case ImageEvidenceKind::CompositeDocument:
                setVerdict(text("PSD composite similarity: %1% · layer equality not verified").arg(score,0,'f',1),"similar");
                break;
            case ImageEvidenceKind::TonemappedHdr:
                setVerdict(text("HDR tonemapped similarity: %1% · source HDR values not verified").arg(score,0,'f',1),"similar");
                break;
            case ImageEvidenceKind::RawEmbeddedPreview:
                setVerdict(text("RAW embedded preview similarity: %1% · sensor data not verified").arg(score,0,'f',1),"similar");
                break;
            case ImageEvidenceKind::RawDeveloped:
                setVerdict(text("Developed RAW similarity: %1% · not a content identity check").arg(score,0,'f',1),"similar");
                break;
            case ImageEvidenceKind::VectorRasterized:
                setVerdict(text("Rasterized SVG similarity: %1% · XML source not verified").arg(score,0,'f',1),"similar");
                break;
            default:
                setVerdict(text("Visually %1% similar · size, format or metadata may differ").arg(score,0,'f',1),"similar");
                break;
            }
        } else {
            if(!a.previewFailed) requestPreview(left);
            if(!b.previewFailed) requestPreview(right);
            setVerdict(text(a.corrupted || b.corrupted || a.previewFailed || b.previewFailed?"Visual similarity unavailable":"Measuring visual similarity…"),"none");
        }
        return;
    }
    if(a.samples.size()==5 && b.samples.size()==5) {
        for(size_t i=0;i<5;++i) if(a.samples[i].valid && b.samples[i].valid) ++valid;
        auto criteria=scanCriteria_; criteria.matchContent=false; criteria.matchSize=false; criteria.matchDuration=false; criteria.matchResolution=false; criteria.matchFileName=false; criteria.matchExtension=false; criteria.crossFolderOnly=false; criteria.matchVisualSamples=true;
        criteria.matchTransforms=options_.matchTransforms; VideoScanner::FilesMatch(a,b,criteria,&score);
    }
    if(valid>=3) {
        setVerdict(text("Visually %1% similar · %2 of 5 moments checked · audio or quality may differ").arg(score,0,'f',1).arg(valid),"similar"); return;
    }
    bool pending=false;
    if(!scanning_ && !closing_ && !fileOperationPending_) for(int index:{left,right}) {
        const auto& file=files_[index];
        if(file.samples.size()!=5 && !file.corrupted && !file.previewFailed && SUCCEEDED(file.error) && (!file.metadataLoaded || file.duration100ns>0)) {
            requestedSamples_.insert(index); requestPreview(index); pending=true;
        }
    }
    setVerdict(text(pending?"Measuring visual similarity…":"Visual similarity unavailable: insufficient samples"),"none");
}

void MainWindow::setVerdict(const QString& value,const char* kind) {
    // Restyle before setting the text so the chip is measured with its final (bold) font.
    if(similarity_->property("verdict").toString()!=QLatin1String(kind)) {
        similarity_->setProperty("verdict",kind); similarity_->style()->unpolish(similarity_); similarity_->style()->polish(similarity_);
    }
    similarity_->setProperty("fullText",value); similarity_->setText(value); similarity_->setToolTip(value);
    // Size the chip to its text; when the row is narrower it shrinks and ends in "…" (see eventFilter), full text stays in the tooltip.
    const int wanted=similarity_->fontMetrics().horizontalAdvance(value)+2*12+4;
    similarity_->setMinimumWidth(std::min(wanted,200)); similarity_->setMaximumWidth(wanted); similarity_->updateGeometry();
    // If the layout keeps the chip's current size no resize follows, so lay it out and fit the new text to it now.
    if(similarity_->isVisible()) {
        if(auto parent=similarity_->parentWidget(); parent && parent->layout()) parent->layout()->activate();
        QResizeEvent fit(similarity_->size(),similarity_->size()); eventFilter(similarity_,&fit);
    }
}
void MainWindow::updatePeers() {
    const int a=left_->fileIndex,b=right_->fileIndex;
    const bool pair=a>=0 && b>=0 && a<int(files_.size()) && b<int(files_.size());
    left_->setPeer(pair?&files_[b]:nullptr); right_->setPeer(pair?&files_[a]:nullptr);
    updateEmptyState();
}

void MainWindow::advanceToNextPair() {
    if(scanning_ || fileOperationPending_ || currentGroup_<0 || currentGroup_>=int(groups_.size())) return;
    const auto& group=groups_[currentGroup_];
    if(group.rightSlot+1<int(group.fileIndices.size())) { selectGroup(currentGroup_,group.rightSlot+1); return; }
    if(const int next=adjacentVisibleGroup(currentGroup_,1); next>=0) { selectGroup(next,1); return; }
    statusBar()->showMessage(text("This was the last pair."));
}

void MainWindow::toggleProtection(PlayerPanel* panel) {
    reviewDirty_=true;
    if(panel->fileIndex<0) return; auto& file=files_[panel->fileIndex]; if(file.protectedByFolder) return; file.protectedFile=!file.protectedFile;
    panel->setFile(&file,panel->fileIndex);
    for(int g=0;g<static_cast<int>(groups_.size());++g) {
        const auto& grp=groups_[g];
        if(std::find(grp.fileIndices.begin(),grp.fileIndices.end(),panel->fileIndex)!=grp.fileIndices.end()) updateGroupRow(g);
    }
}

void MainWindow::refreshReviewDecisions() {
    ScanCache cache(cachePath_); auto decisions=cache.reviewDecisions();
    if(!cache.error().isEmpty()) { cacheError_=cache.error(); return; }
    reviewDecisions_=std::move(decisions); deferredReviewFiles_.clear();
    for(const auto& [pair,decision]:reviewDecisions_) if(decision==ReviewDecision::KeepBoth || decision==ReviewDecision::Later) {
        const auto separator=pair.find('/');
        if(separator!=std::string::npos) { deferredReviewFiles_.insert(pair.substr(0,separator)); deferredReviewFiles_.insert(pair.substr(separator+1)); }
    }
}
bool MainWindow::protectedByReview(const VideoFileInfo& file) const {
    const auto id=FileReviewId(file);
    if(!id.empty() && deferredReviewFiles_.contains(id)) return true;
    const auto legacy=FileReviewId(file.identity);
    return !legacy.empty() && deferredReviewFiles_.contains(legacy);
}
ReviewDecision MainWindow::pairDecision(int a,int b) const {
    if(a<0 || b<0 || a>=int(files_.size()) || b>=int(files_.size())) return ReviewDecision::Unreviewed;
    if(auto found=reviewDecisions_.find(ReviewPairKey(files_[a],files_[b])); found!=reviewDecisions_.end()) return found->second;
    const auto legacy=reviewDecisions_.find(ReviewPairKey(files_[a].identity,files_[b].identity));
    return legacy==reviewDecisions_.end()?ReviewDecision::Unreviewed:legacy->second;
}
ReviewDecision MainWindow::currentReviewDecision() const {
    return pairDecision(left_->fileIndex,right_->fileIndex);
}
void MainWindow::updateReviewState() {
    const auto a=left_->fileIndex,b=right_->fileIndex;
    const bool active=!scanning_ && !fileOperationPending_ && !closing_ && a>=0 && b>=0 && a<int(files_.size()) && b<int(files_.size());
    auto decision=active?currentReviewDecision():ReviewDecision::Unreviewed;
    for(int i=1;i<4;++i) {
        reviewActions_[i]->setEnabled(active); reviewActions_[i]->setChecked(i==int(decision));
        reviewActions_[i]->setToolTip(i==int(decision)?text("Click again to clear the decision."):QString());
    }
    reviewActions_[0]->setEnabled(active && decision!=ReviewDecision::Unreviewed);
    if(auto mark=tickTargets_.resolved?tickTargets_.markRight:findChild<QAction*>("Mark right file for cleanup")) {
        mark->setEnabled(active && !files_[b].protectedFile && !protectedByReview(files_[b])); mark->setChecked(active && markedFiles_.contains(b));
    }
    if(cleanupMarkedButton_) {
        // This runs on the playback tick, so only real changes are applied.
        const auto label=text("Clean up marked (%1)…").arg(markedFiles_.size());
        if(cleanupMarkedButton_->text()!=label) {
            cleanupMarkedButton_->setText(label); cleanupMarkedButton_->setAccessibleName(label);
            cleanupMarkedButton_->setToolTip(text("Move marked files to Recycle Bin…"));
        }
        cleanupMarkedButton_->setVisible(!markedFiles_.isEmpty()); cleanupMarkedButton_->setEnabled(!scanning_ && !fileOperationPending_ && !closing_);
    }
    if(active) {
        showGroupPosition(decision==ReviewDecision::Unreviewed?QString():text(reviewLabels[int(decision)]));
        for(auto panel:{left_,right_}) {
            const auto& file=files_[panel->fileIndex]; const bool held=protectedByReview(file);
            panel->setRemovalEnabled(!file.protectedFile && !held,held?text("Protected by a keep or review-later decision. Clear the relevant pair decision to allow removal."):QString());
        }
    }
}
void MainWindow::setReviewDecision(ReviewDecision decision) {
    if(scanning_ || sortingMetadata_ || fileOperationPending_ || clearingCache_ || closing_ || currentGroup_<0 || left_->fileIndex<0 || right_->fileIndex<0) return;
    const auto& a=files_[left_->fileIndex]; const auto& b=files_[right_->fileIndex];
    if(FileOperations::ReadIdentity(a.fullPath)!=a.identity || FileOperations::ReadIdentity(b.fullPath)!=b.identity) {
        statusBar()->showMessage(text("This file changed or is no longer available. Rescan before removing it.")); return;
    }
    ScanCache cache(cachePath_); cache.saveReviewDecisions({{ReviewPairKey(a,b),decision}});
    if(!cache.error().isEmpty()) { plainWarning(this,text("Review decision"),cache.error()); return; }
    refreshReviewDecisions();
    // A keep or review-later decision protects both files, so cleanup marks on them would only be skipped later.
    for(auto it=markedFiles_.begin();it!=markedFiles_.end();) { if(protectedByReview(files_[*it])) it=markedFiles_.erase(it); else ++it; }
    updateGroupRow(currentGroup_);
    updateReviewState();
}

void MainWindow::ignoreCurrentPair() {
    if(scanning_ || sortingMetadata_ || fileOperationPending_ || clearingCache_ || closing_ || left_->fileIndex<0 || right_->fileIndex<0) return;
    ScanCache cache(cachePath_); cache.ignorePair(files_[left_->fileIndex],files_[right_->fileIndex]);
    if(cache.error().isEmpty()) scanCriteria_.ignoredPairs=cache.ignoredPairs();
    if(!cache.error().isEmpty()) { plainWarning(this,text("Scan report"),cache.error()); return; }
    if(manual_) { groups_.clear(); currentGroup_=-1; left_->setFile(nullptr,-1); right_->setFile(nullptr,-1); populateResults(); return; }
    if(currentGroup_<0 || currentGroup_>=int(groups_.size())) return;
    // Regroup only the affected group in memory; a rescan would discard cleanup marks and the review position.
    const int groupIndex=currentGroup_,previousLeft=left_->fileIndex;
    auto members=groups_[groupIndex].fileIndices; std::erase_if(members,[this](int index) { return files_[index].deleted; });
    std::vector<VideoGroup> replacement; std::atomic<bool> cancel{false};
    VideoScanner::GroupKnownFiles(files_,std::move(members),scanCriteria_,replacement,cancel);
    groups_.erase(groups_.begin()+groupIndex);
    groups_.insert(groups_.begin()+groupIndex,replacement.begin(),replacement.end());
    // Only the affected row is rebuilt; rebuilding every row made Ignore take seconds on long result lists.
    currentGroup_=-1; replaceGroupRows(groupIndex,int(replacement.size())); reviewDirty_=true;
    if(groups_.empty()) { left_->setFile(nullptr,-1); right_->setFile(nullptr,-1); right_->setCandidates(nullptr); groupLabel_->setText(text("No active group")); updateSimilarity(); }
    else {
        int target=std::min(groupIndex,int(groups_.size())-1);
        for(int g=groupIndex;g<groupIndex+int(replacement.size());++g) if(groups_[g].fileIndices.front()==previousLeft) { target=g; break; }
        selectGroup(target);
    }
    statusBar()->showMessage(text("Pair ignored. Cleanup marks and the other results were kept."));
}
