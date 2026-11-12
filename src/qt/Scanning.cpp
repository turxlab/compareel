#include "MainWindow.h"
#include <map>
#include "MainWindowInternal.h"
#include "Language.h"
#include "Icons.h"
#include "Theme.h"
#include "FileOperations.h"
#include "platform/Platform.h"
#include "ScanCache.h"
#include "ScanManifest.h"
#include "Csv.h"
#include "ImageReader.h"
#include "ScanConcurrency.h"
#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QMessageBox>
#include <QPixmapCache>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QThread>
#include <QToolButton>
#include <QVBoxLayout>

using Language::text;

ScanCriteria MainWindow::activeCriteria() const {
    ScanCriteria criteria=options_; criteria.loadPreviews=false; criteria.comparisonRoots=folders_;
    criteria.matchContent=profile_->currentIndex()==0; criteria.matchVisualSamples=profile_->currentIndex()==1;
    if(criteria.matchContent) { criteria.matchSize=true; criteria.matchDuration=false; criteria.matchResolution=false; criteria.matchFirstFrame=false; criteria.matchFileName=false; criteria.matchExtension=false; }
    if(criteria.matchVisualSamples) { criteria.matchSize=false; criteria.matchDuration=true; criteria.matchResolution=false; criteria.matchFirstFrame=false; criteria.matchFileName=false; criteria.matchExtension=false; }
    if(criteria.images) {
        criteria.matchDuration=false; criteria.matchVisualSamples=false;
        if(profile_->currentIndex()==1) { criteria.matchSize=false; criteria.matchResolution=false; criteria.matchFirstFrame=true; criteria.matchFileName=false; criteria.matchExtension=false; }
    }
    return criteria;
}

void MainWindow::startScan(bool resume) {
    if(scanning_ || clearingCache_ || closing_ || fileOperationPending_ || sortingMetadata_) return;
    if(folders_.empty() && !restoredFiles_) { chooseFolders(); return; }
    const bool snapshot=restoredGroups_.has_value();
    if(!snapshot) scanElapsed_.start();
    if(scanThread_.joinable()) scanThread_.join();
    // The previous results leave the window at once; they are not shown behind a new scan.
    clearResults();
    scanCriteria_=activeCriteria(); cancel_.store(false); scanning_=true; ++generation_; pendingPreviews_.clear(); previewRuns_.clear(); requestedSamples_.clear();
    updateScanTimeLabel();
    previewPool_.waitForDone();
    pausedScan_=false; scanPausable_=!snapshot && (!folders_.empty() || (restoredFiles_ && !restoreManual_)); saveSettings();
    settings_.setValue("scanInProgress",!snapshot && !folders_.empty()); settings_.sync();
    pauseAction_->setEnabled(scanPausable_); resumeAction_->setEnabled(false);
    updateSourceSummary();
    foldersAction_->setEnabled(false); rescanAction_->setEnabled(false); stopAction_->setEnabled(true); profile_->setEnabled(false); if(similarityThreshold_) similarityThreshold_->setEnabled(false); sort_->setEnabled(false); results_->setEnabled(false);
    progress_->setRange(0,0); progress_->show(); statusBar()->showMessage(text(snapshot?"Checking saved results…":"Scanning…")); updateEmptyState();
    auto roots=folders_; auto criteria=scanCriteria_; auto generation=generation_; auto cachePath=cachePath_;
    auto restored=std::move(restoredFiles_); restoredFiles_.reset(); bool restoredManual=restoreManual_; QPointer<MainWindow> self(this);
    auto savedGroups=std::move(restoredGroups_); restoredGroups_.reset();
    auto savedIssues=std::move(restoredIssues_); restoredIssues_.clear();
    auto savedMetrics=std::move(restoredMetrics_); restoredMetrics_={};
    scanThread_=std::thread([self,roots,criteria,generation,cachePath,resume,restored=std::move(restored),savedGroups=std::move(savedGroups),savedIssues=std::move(savedIssues),savedMetrics=std::move(savedMetrics),restoredManual,this] () mutable {
        Platform::ComInit com(Platform::ComApartment::MultiThreaded);
        auto result=std::make_shared<ScanResult>();
        try {
            if(savedGroups && restored) {
                ScanCache cache(cachePath);
                result->ignoredPairs=cache.ignoredPairs();
                result->restored=true; result->manual=restoredManual; result->files=std::move(*restored); result->groups=std::move(*savedGroups);
                result->issues=std::move(savedIssues); result->metrics=std::move(savedMetrics);
                QSet<int> checked;
                for(auto& group:result->groups) {
                    const int selected=group.fileIndices[group.rightSlot];
                    for(int index:group.fileIndices) {
                        if(cancel_.load()) break;
                        if(checked.contains(index)) continue; checked.insert(index);
                        auto& file=result->files[index];
                        if(FileOperations::ReadIdentity(file.fullPath)!=file.identity) {
                            ++result->changedFiles;
                            file.deleted=true; result->issues.push_back({file.fullPath,HRESULT_FROM_WIN32(ERROR_FILE_INVALID)});
                        }
                    }
                    std::erase_if(group.fileIndices,[&](int index) { return result->files[index].deleted; });
                    auto choice=std::find(group.fileIndices.begin(),group.fileIndices.end(),selected);
                    group.rightSlot=choice!=group.fileIndices.end() && choice!=group.fileIndices.begin()?int(choice-group.fileIndices.begin()):1;
                    group.bestFrameSim=group.fileIndices.size()>=2?VideoScanner::ComputePairSimilarity(result->files[group.fileIndices[0]],result->files[group.fileIndices[group.rightSlot]],criteria.matchTransforms):0.0;
                }
                std::erase_if(result->groups,[](const auto& group) { return group.fileIndices.size()<2; });
            } else {
            const auto manifest=cachePath+".scan-job";
            const bool frozen=resume && !restored && QFileInfo::exists(manifest);
            if(frozen) {
                restoredManual=false;
                restored.emplace();
                if(!ScanManifest::load(manifest,criteria,*restored)) throw std::runtime_error("Saved scan list is invalid or scan settings changed. Start a new scan.");
            } else if(!resume && !restored && QFileInfo::exists(manifest) && !QFile::remove(manifest)) {
                throw std::runtime_error("The previous scan list could not be replaced. Check the data folder permissions.");
            }
            if(restored && !restoredManual && !roots.empty() && !frozen) {
                ScanManifest::SaveStatus manifestStatus=ScanManifest::SaveStatus::Saved;
                if(!ScanManifest::save(manifest,criteria,*restored,&manifestStatus)) {
                    if(manifestStatus==ScanManifest::SaveStatus::TooLarge) {
                        QFile::remove(manifest);
                        if(self) QMetaObject::invokeMethod(self,[self,generation] {
                            if(self && self->generation_==generation && self->scanning_) { self->scanPausable_=false; self->pauseAction_->setEnabled(false); }
                        },Qt::QueuedConnection);
                    } else {
                        throw std::runtime_error("The saved session could not be checkpointed for scanning.");
                    }
                }
            }
            auto progress=[self,generation,images=criteria.images,hashing=criteria.matchContent](const ScanProgress& value) {
                if(!self) return;
                QMetaObject::invokeMethod(self,[self,generation,value,images,hashing] {
                    if(!self || self->generation_!=generation || !self->scanning_) return;
                    // Stage 2 hashes contents for exact copies but only reads media information for similarity scans.
                    const char* stage=value.stage==1?"Reading files":value.stage==2?(hashing?"Verifying contents":"Reading media information"):value.stage==3?(images?"Reading images":"Sampling videos"):"Grouping matches";
                    self->statusBar()->showMessage(text(stage)+QString("   %1 / %2").arg(value.current).arg(value.total));
                    // Finding files has no known total, so that stage keeps the bar moving instead of showing it full.
                    if(value.stage==1) self->progress_->setRange(0,0);
                    else if(value.total>0) { self->progress_->setRange(0,value.total); self->progress_->setValue(value.current); }
                },Qt::QueuedConnection);
            };
            if(restored) {
                result->files=std::move(*restored); result->manual=restoredManual;
                result->metrics.discoveredFiles=result->files.size();
                for(auto& file:result->files) {
                    if(cancel_.load()) break;
                    if(FileOperations::ReadIdentity(file.fullPath)!=file.identity) {
                        file.deleted=true; result->issues.push_back({file.fullPath,HRESULT_FROM_WIN32(ERROR_FILE_INVALID)});
                    }
                }
            } else {
                result->files=VideoScanner::DiscoverVideos(roots,criteria.includeSubfolders,cancel_,progress,&criteria,&result->issues);
                result->metrics.discoveredFiles=result->files.size();
                if(!cancel_.load()) {
                    ScanManifest::SaveStatus manifestStatus=ScanManifest::SaveStatus::Saved;
                    if(!ScanManifest::save(manifest,criteria,result->files,&manifestStatus)) {
                        if(manifestStatus==ScanManifest::SaveStatus::TooLarge && (criteria.matchContent || criteria.matchSize)) {
                            std::map<uint64_t,unsigned> sizes;
                            size_t outsideLimits=0;
                            for(const auto& f:result->files) {
                                if(f.deleted) continue;
                                if(f.fileSize<criteria.minFileSize || f.fileSize>criteria.maxFileSize) ++outsideLimits;
                                else ++sizes[f.fileSize];
                            }
                            const size_t beforePrune=result->files.size();
                            std::erase_if(result->files,[&](const VideoFileInfo& f) {
                                if(f.deleted) return true;
                                if(f.fileSize<criteria.minFileSize || f.fileSize>criteria.maxFileSize) return true;
                                return sizes[f.fileSize]<2;
                            });
                            result->metrics.outsideSizeLimits+=outsideLimits;
                            result->metrics.skippedUniqueSize+=(beforePrune-outsideLimits-result->files.size());
                            result->metrics.compactedFiles=true;
                            ScanManifest::save(manifest,criteria,result->files,&manifestStatus);
                        }
                        if(manifestStatus==ScanManifest::SaveStatus::TooLarge) {
                            QFile::remove(manifest);
                            if(self) QMetaObject::invokeMethod(self,[self,generation] {
                                if(self && self->generation_==generation && self->scanning_) { self->scanPausable_=false; self->pauseAction_->setEnabled(false); }
                            },Qt::QueuedConnection);
                        } else if(manifestStatus!=ScanManifest::SaveStatus::Saved) {
                            throw std::runtime_error("The scan list could not be saved. Check the data folder permissions.");
                        }
                    }
                }
            }
            ScanCache cache(cachePath);
            criteria.ignoredPairs=cache.ignoredPairs();
            result->ignoredPairs=criteria.ignoredPairs;
            result->cacheHits=cache.restoreBatch(result->files,cancel_);
            CacheWriter checkpoints(cachePath);
            if(result->manual) {
                VideoGroup group; for(int i=0;i<static_cast<int>(result->files.size());++i) if(!result->files[i].deleted) group.fileIndices.push_back(i);
                if(group.fileIndices.size()==2) result->groups.push_back(std::move(group));
            } else VideoScanner::AnalyzeAndGroup(result->files,criteria,result->groups,cancel_,progress,[&](const auto& file) { checkpoints.submit(file); },&result->metrics,[&](auto& file) {
                if(file.frameLoaded && !file.metadataLoaded && file.width>0 && file.height>0) StillImages::refreshCaptureTime(file,cancel_);
                else StillImages::analyze(file,cancel_);
            });
            checkpoints.finish(); result->cacheError=cache.error().isEmpty()?checkpoints.error():cache.error();
            if(!cancel_.load() && !result->manual) {
                std::vector<int> remap(result->files.size(),-1);
                size_t groupedCount=0;
                for(const auto& group:result->groups) {
                    for(int idx:group.fileIndices) {
                        if(idx>=0 && size_t(idx)<remap.size() && remap[idx]!=0) { remap[idx]=0; ++groupedCount; }
                    }
                }
                size_t approxBytes=4096+result->issues.size()*160;
                for(size_t i=0;i<result->files.size();++i) {
                    const auto& f=result->files[i];
                    if(f.deleted) continue;
                    approxBytes+=150+f.fullPath.size()*2+(remap[i]==0?(f.samples.empty()?1600:2800):0);
                }
                const bool canPruneUnmatched=groupedCount<result->files.size() && result->files.size()>512;
                if(result->files.size()>100000 || (canPruneUnmatched && approxBytes>22*1024*1024)) {
                    result->metrics.compactedFiles=true;
                    if(criteria.matchVisualSamples) {
                        for(const auto& file:result->files) {
                            if(!file.deleted && !file.corrupted && file.samples.size()==5 &&
                               std::count_if(file.samples.begin(),file.samples.end(),[](const auto& sample) { return sample.valid; })<3) {
                                result->metrics.lowVarianceSkipped.push_back(file.fullPath);
                            }
                        }
                    }
                    std::vector<VideoFileInfo> kept;
                    for(size_t i=0;i<result->files.size();++i) {
                        auto& f=result->files[i];
                        if(remap[i]==0 && !f.deleted) {
                            remap[i]=int(kept.size());
                            kept.push_back(std::move(f));
                        } else {
                            remap[i]=-1;
                            if(!f.deleted && FAILED(f.error)) result->issues.push_back({f.fullPath,f.error});
                        }
                    }
                    for(auto& group:result->groups) {
                        for(int& idx:group.fileIndices) idx=remap[idx];
                        std::erase_if(group.fileIndices,[](int idx) { return idx<0; });
                    }
                    std::erase_if(result->groups,[](const VideoGroup& g) { return g.fileIndices.size()<2; });
                    result->files=std::move(kept);
                }
            }
            }
            result->cancelled=cancel_.load();
            if(!result->cancelled && !restoredManual && !result->restored) QFile::remove(cachePath+".scan-job");
        } catch(const std::exception& error) { result->error=QString::fromUtf8(error.what()); }
        if(self) QMetaObject::invokeMethod(self,[self,result,generation] { if(self) self->receiveScan(result,generation); },Qt::QueuedConnection);
    });
}

void MainWindow::receiveScan(std::shared_ptr<ScanResult> result,uint64_t generation) {
    if(generation!=generation_) return;
    if(scanThread_.joinable()) scanThread_.join();
    scanning_=false; foldersAction_->setEnabled(true); rescanAction_->setEnabled(true); stopAction_->setEnabled(false); profile_->setEnabled(true); if(similarityThreshold_) similarityThreshold_->setEnabled(true); sort_->setEnabled(true); results_->setEnabled(true); progress_->hide();
    scanPausable_=false; pauseAction_->setEnabled(false);
    if(folders_.empty() && !QFileInfo::exists(cachePath_+".scan-job")) pausedScan_=false;
    resumeAction_->setEnabled(pausedScan_); updateEmptyState();
    if(pendingClose_) { if(!result->restored) finishScanTiming(result->cancelled?(pausedScan_?"paused":"stopped"):result->error.isEmpty()?"completed":"failed"); close(); return; }
    cacheHits_=result->cacheHits; cacheError_=result->cacheError; scanMetrics_=result->metrics;
    if(result->cancelled) { if(!result->restored) finishScanTiming(pausedScan_?"paused":"stopped"); if(currentGroup_>=0) selectGroup(currentGroup_); statusBar()->showMessage(text(pausedScan_?"Scan paused; completed reads are saved.":"Scan stopped")); saveSettings(); return; }
    if(!result->error.isEmpty()) { finishScanTiming("failed"); QMessageBox::warning(this,text("Scan report"),text(result->error.toUtf8().constData())); return; }
    manual_=result->manual; markedFiles_.clear(); files_=std::move(result->files); groups_=std::move(result->groups); scanIssues_=std::move(result->issues); scanCriteria_.ignoredPairs=std::move(result->ignoredPairs); currentGroup_=-1;
    savedResultEvidence_=result->restored;
    if(!result->restored) applyReferenceRule();
    for(int i=0;i<int(files_.size());++i) if(!files_[i].deleted) {
        if(restoreMarks_.contains(FileReviewId(files_[i])) || restoreMarks_.contains(FileReviewId(files_[i].identity)))
            markedFiles_.insert(i);
    }
    restoreMarks_.clear();
    if(!restoreRightCandidates_.empty()) {
        for(auto& group:groups_) {
            if(group.fileIndices.empty()) continue;
            auto leftPath=QString::fromStdWString(files_[group.fileIndices[0]].fullPath);
            auto rightPath=restoreRightCandidates_.value(leftPath);
            if(rightPath.isEmpty()) continue;
            for(int s=1;s<static_cast<int>(group.fileIndices.size());++s) {
                if(QString::fromStdWString(files_[group.fileIndices[s]].fullPath)==rightPath) { group.rightSlot=s; break; }
            }
        }
        restoreRightCandidates_.clear();
    }
    bool restoredActiveGroup=false;
    if(!restoreLeft_.isEmpty()) {
        for(int g=0;g<static_cast<int>(groups_.size());++g) {
            auto& members=groups_[g].fileIndices;
            auto anchor=std::find_if(members.begin(),members.end(),[&](int i) { return QString::fromStdWString(files_[i].fullPath)==restoreLeft_; });
            if(anchor==members.end()) continue;
            if(!manual_ && !options_.images && !std::all_of(members.begin(),members.end(),[&](int i) { return i==*anchor || VideoScanner::FilesMatch(files_[*anchor],files_[i],scanCriteria_); })) break;
            if(anchor!=members.begin()) std::rotate(members.begin(),anchor,anchor+1);
            int comparison=1; for(int s=1;s<static_cast<int>(members.size());++s) if(QString::fromStdWString(files_[members[s]].fullPath)==restoreRight_) comparison=s;
            groups_[g].rightSlot=comparison;
            populateResults(); selectGroup(g,comparison); left_->canvas->setTransform(restoreZoom_,restorePan_); right_->canvas->setTransform(restoreZoom_,restorePan_);
            { QSignalBlocker blocker(offset_); offset_->setValue(restoreOffset_); alignmentOffset100ns_=static_cast<int64_t>(std::llround(restoreOffset_*10000000)); }
            linkAction_->setChecked(restoreLinked_);
                restorePositions_={{restoreLeftTime_,restoreRightTime_}};
            restoredActiveGroup=true; break;
        }
        restoreLeft_.clear(); restoreRight_.clear();
    }
    if(!restoredActiveGroup) {
        populateResults();
        if(!groups_.empty()) selectGroup(0); else { left_->setFile(nullptr,-1); right_->setFile(nullptr,-1); right_->setCandidates(nullptr); statusBar()->showMessage(text("No matches found")+" · "+text("Try another profile or check the scan folders.")); }
    }
    QTimer::singleShot(0,this,[this] { requestVisiblePreviews(); });
    size_t errors=scanIssues_.size()+std::count_if(files_.begin(),files_.end(),[](const auto& file) { return FAILED(file.error); });
    if(errors) statusBar()->showMessage(text("%1 files could not be read").arg(errors));
    else if(!groups_.empty()) statusBar()->showMessage(cacheHits_?text("Ready")+" · "+text("%1 files reused from cache").arg(cacheHits_):text("Ready"));
    if(result->restored) {
        settings_.setValue("scanInProgress",false); saveSettings(); updateScanTimeLabel();
        statusBar()->showMessage(text("Saved results opened · %1 changed or missing files excluded").arg(result->changedFiles));
    } else { finishScanTiming("completed"); if(groups_.empty()) updateEmptyState(); }
    if(!result->restored && !manual_ && !saveScanReportFile(QFileInfo(settings_.fileName()).dir().filePath("last-scan-report.txt"),scanReportText()))
        statusBar()->showMessage(text("The scan report could not be saved."));
}

void MainWindow::finishScanTiming(const QString& outcome) {
    lastScanElapsedMs_=scanElapsed_.isValid()?scanElapsed_.elapsed():0;
    scanElapsed_.invalidate(); lastScanOutcome_=outcome; scanCompletedThisSession_=outcome=="completed";
    settings_.setValue("scanInProgress",false);
    settings_.setValue("lastScanElapsedMs",lastScanElapsedMs_); settings_.setValue("lastScanOutcome",lastScanOutcome_); settings_.sync();
    updateScanTimeLabel();
}

void MainWindow::updateScanTimeLabel() {
    QString label;
    if(scanning_ && scanElapsed_.isValid()) label=text("Elapsed: %1").arg(scanDuration(scanElapsed_.elapsed()));
    else if(!lastScanOutcome_.isEmpty()) {
        const char* format=lastScanOutcome_=="completed"?"Last scan completed · %1":lastScanOutcome_=="stopped"?"Last scan stopped · %1":
            lastScanOutcome_=="paused"?"Last scan paused · %1":"Last scan failed · %1";
        label=text(format).arg(scanDuration(lastScanElapsedMs_));
    }
    scanTimeLabel_->setText(label); scanTimeLabel_->setVisible(!label.isEmpty());
}

QString MainWindow::scanReportText() const {
    QStringList errors; QMap<QString,int> byType;
    auto addError=[&](const std::wstring& path,HRESULT error) {
        errors<<QString::fromStdWString(path)+"\n"+readError(error);
        auto extension=QFileInfo(QString::fromStdWString(path)).suffix().toLower();
        if(extension.isEmpty()) extension="-";
        ++byType[extension+QString(" · 0x%1").arg(quint32(error),8,16,QChar('0'))];
    };
    for(const auto& file:files_) if(FAILED(file.error)) addError(file.fullPath,file.error);
    for(const auto& issue:scanIssues_) addError(issue.path,issue.error);
    QString summary=QDateTime::currentDateTime().toString(Qt::ISODate)+"\n";
    const size_t totalFiles=scanMetrics_.compactedFiles && scanMetrics_.discoveredFiles>0?scanMetrics_.discoveredFiles:files_.size();
    summary+=text("%1 files · %2 read errors").arg(totalFiles).arg(errors.size())+"\n";
    // Files that cannot have a match are left out before reading; the report says so, so they are not taken for failures.
    if(!manual_) {
        size_t outsideLimits=0,uniqueSize=0;
        if(scanMetrics_.compactedFiles) {
            outsideLimits=scanMetrics_.outsideSizeLimits;
            uniqueSize=scanMetrics_.skippedUniqueSize;
        } else {
            std::map<uint64_t,unsigned> sizes;
            for(const auto& file:files_) {
                if(file.fileSize<scanCriteria_.minFileSize || file.fileSize>scanCriteria_.maxFileSize) ++outsideLimits; else ++sizes[file.fileSize];
            }
            if(scanCriteria_.matchContent || scanCriteria_.matchSize) for(const auto& [size,count]:sizes) if(count==1) ++uniqueSize;
        }
        if(outsideLimits) summary+=text("Not compared: %1 files outside the size limits in Scan rules.").arg(outsideLimits)+"\n";
        if(uniqueSize) summary+=text("Not compared: %1 files with a size no other file has; they cannot have an exact copy.").arg(uniqueSize)+"\n";
    }
    const auto fileCount=[&] { QSet<int> indices; for(const auto& group:groups_) for(int i:group.fileIndices) indices.insert(i); return indices.size(); }();
    summary+=(groups_.size()==1?text("1 group · %1 files").arg(fileCount):text("%1 groups · %2 files").arg(groups_.size()).arg(fileCount))+"\n";
    for(auto it=byType.cbegin();it!=byType.cend();++it) summary+=it.key()+": "+QString::number(it.value())+"\n";
    summary+="\n";
    if(scanCriteria_.matchVisualSamples) {
        const auto lowVarNote=text("The picture hardly changes (a still image or slides), so it was not compared visually, to avoid false matches. The \"Exact copies\" mode still finds its exact copies.");
        if(scanMetrics_.compactedFiles && !scanMetrics_.lowVarianceSkipped.empty()) {
            for(const auto& p:scanMetrics_.lowVarianceSkipped) errors<<QString::fromStdWString(p)+"\n"+lowVarNote;
        } else {
            for(const auto& file:files_) if(!file.deleted && !file.corrupted && file.samples.size()==5 && std::count_if(file.samples.begin(),file.samples.end(),[](const auto& sample) { return sample.valid; })<3)
                errors<<QString::fromStdWString(file.fullPath)+"\n"+lowVarNote;
        }
    }
    if(!cacheError_.isEmpty()) errors<<text("Cache unavailable; scanning still works.")+"\n"+cacheError_;
    if(scanCriteria_.matchVisualSamples && !manual_) {
        summary+=text("Compared visually: %1 of %2 videos. Not compared: %3 videos with no other video of a similar length; they cannot have a similar copy.").arg(scanMetrics_.sampleCandidates).arg(scanMetrics_.inputCandidates).arg(scanMetrics_.durationRejected)+"\n";
        summary+=text("Reading information: %1 s · Sampling: %2 s · Grouping: %3 s")
            .arg(scanMetrics_.readMs/1000.0,0,'f',2).arg(scanMetrics_.sampleMs/1000.0,0,'f',2).arg(scanMetrics_.groupMs/1000.0,0,'f',2)+"\n\n";
    }
    if(!manual_ && scanMetrics_.requestedWorkers) {
        summary+=text("Parallel read limit: requested %1 · information/hash %2 · video %3. CPU and available memory may reduce the limit.")
            .arg(scanMetrics_.requestedWorkers).arg(scanMetrics_.readWorkers).arg(scanMetrics_.sampleWorkers)+"\n";
        summary+=text("Detected storage tier: %1 · per-volume active limit: %2.")
            .arg(text(StorageTierKey(scanMetrics_.primaryStorageTier))).arg(scanMetrics_.volumeCap)+"\n\n";
    }

    return summary+(errors.empty()?text("No errors reported."):errors.join("\n\n"));
}
bool MainWindow::saveScanReportFile(const QString& path,const QString& body) const {
    QSaveFile output(path); if(!output.open(QIODevice::WriteOnly)) return false;
    const auto bytes=body.toUtf8(); return output.write(bytes)==bytes.size() && output.commit();
}
void MainWindow::showScanReport() {
    QDialog dialog(this); dialog.setWindowTitle(text("Scan report")); dialog.resize(760,420); auto layout=new QVBoxLayout(&dialog);
    auto report=new QPlainTextEdit(&dialog); report->setReadOnly(true);
    QString body=scanReportText();
    if(files_.empty() && scanIssues_.empty()) {
        QFile saved(QFileInfo(settings_.fileName()).dir().filePath("last-scan-report.txt"));
        if(saved.open(QIODevice::ReadOnly) && saved.size()<=32*1024*1024) body=QString::fromUtf8(saved.readAll());
    }
    report->setPlainText(body); layout->addWidget(report);
    auto buttons=new QDialogButtonBox(QDialogButtonBox::Close,&dialog); buttons->button(QDialogButtonBox::Close)->setText(text("Close"));
    auto save=buttons->addButton(text("Save scan report…"),QDialogButtonBox::ActionRole);
    connect(save,&QPushButton::clicked,&dialog,[this,&dialog,report] {
        auto path=QFileDialog::getSaveFileName(&dialog,text("Save scan report…"),{},"Text (*.txt)");
        if(!path.isEmpty() && !saveScanReportFile(path,report->toPlainText())) QMessageBox::warning(&dialog,text("Scan report"),text("The scan report could not be saved."));
    });
    connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject); layout->addWidget(buttons); dialog.exec();
}

void MainWindow::exportResults() {
    if(scanning_) return;
    auto path=QFileDialog::getSaveFileName(this,text("Export results…"),{},"CSV (*.csv)");
    if(!path.isEmpty() && !exportResultsFile(path)) QMessageBox::warning(this,text("Export results…"),text("The export could not be saved."));
}
bool MainWindow::exportResultsFile(const QString& path) {
    if(scanning_) return false;
    QSaveFile output(path); if(!output.open(QIODevice::WriteOnly)) return false;
    QByteArray bytes=QByteArray::fromHex("efbbbf")+"group,role,path,bytes,duration_seconds,width,height,evidence,protected,marked_for_recycle\r\n";
    for(int g=0;g<static_cast<int>(groups_.size());++g) {
        auto& members=groups_[g].fileIndices;
        for(int slot=0;slot<static_cast<int>(members.size());++slot) {
            const auto& file=files_[members[slot]];
            QString evidence=csvEvidenceTag(file,scanCriteria_,manual_);
            QStringList row{QString::number(g+1),QString("slot_%1").arg(slot+1),QString::fromStdWString(file.fullPath),QString::number(file.fileSize),QString::number(file.DurationSeconds(),'f',6),QString::number(file.width),QString::number(file.height),evidence,file.protectedFile?"true":"false",markedFiles_.contains(members[slot])?"true":"false"};
            for(auto& value:row) value=Csv::field(value); bytes+=row.join(',').toUtf8()+"\r\n";
        }
    }
    return output.write(bytes)==bytes.size() && output.commit();
}
