#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "ResultsDelegate.h"
#include "Language.h"
#include "Icons.h"
#include "Theme.h"
#include "FileOperations.h"
#include "ScanCache.h"
#include "ImageReader.h"
#include "FFmpegReader.h"
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPointer>
#include <QProgressDialog>
#include <QPushButton>
#include <QScreen>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSlider>
#include <QSplitter>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWindow>
#include <QtConcurrent/QtConcurrentRun>
#include <QHeaderView>

using Language::text;

MainWindow::MainWindow(const QString& dataDirectory,QWidget* parent) :
    QMainWindow(parent), settings_(settingsPath(dataDirectory),QSettings::IniFormat) {
    setObjectName("CompareelWindow"); setWindowTitle("Compareel "+QCoreApplication::applicationVersion()); resize(1360,920); setMinimumSize(1040,720); setAcceptDrops(true);
    Language::setCurrent(settings_.value("language",Language::defaultCode()).toString());
    Theme::apply(*qApp,settings_.value("theme","dark").toString());
    FFmpegReader::SetGpuEnabled(settings_.value("preferGpuDecode",false).toBool());
    options_.images=settings_.value("images",false).toBool();
    folders_=paths(settings_.value("folders").toStringList());
    options_.excludedFolders=paths(settings_.value("excluded").toStringList());
    options_.protectedFolders=paths(settings_.value("protected").toStringList());
    options_.includeSubfolders=settings_.value("recursive",true).toBool();
    options_.workerCount=settings_.value("workers",2).toUInt();
    options_.minFileSize=settings_.value("minSize",1).toULongLong();
    options_.maxFileSize=settings_.value("maxSize",qulonglong(UINT64_MAX)).toULongLong();
    options_.extensions=paths(settings_.value(options_.images?"imageExtensions":"extensions").toStringList());
    options_.excludedExtensions=paths(settings_.value(options_.images?"imageExcludedExtensions":"excludedExtensions").toStringList());
    options_.nonRecursiveFolders=paths(settings_.value("nonRecursiveFolders").toStringList());
    options_.minFrameSimilarity=std::clamp(settings_.value("similarity",90).toDouble(),70.0,100.0);
    options_.durationTol100ns=std::clamp<int64_t>(settings_.value("durationTolerance",2000000).toLongLong(),0,600000000);
    options_.matchSize=settings_.value("matchSize",true).toBool(); options_.matchDuration=settings_.value("matchDuration",false).toBool();
    options_.matchResolution=settings_.value("matchResolution",false).toBool(); options_.matchFirstFrame=settings_.value("matchFirstFrame",false).toBool();
    options_.matchFileName=settings_.value("matchFileName",false).toBool(); options_.matchExtension=settings_.value("matchExtension",false).toBool();
    options_.sortOrder=static_cast<SortOrder>(std::clamp(settings_.value("sort",0).toInt(),0,6));
    previewPool_.setMaxThreadCount(2); cachePath_=QFileInfo(settings_.fileName()).dir().filePath("scan-cache-v1.sqlite");
    cleanupReviewPath_=QFileInfo(settings_.fileName()).dir().filePath("last-cleanup.cvsession");
    lastReviewPath_=QFileInfo(settings_.fileName()).dir().filePath("last-review.cvsession");
    refreshReviewDecisions();
    options_.loadPreviews=false;
    options_.matchTransforms=settings_.value("visualTransforms",true).toBool();
    options_.crossFolderOnly=settings_.value("crossFolderOnly",false).toBool();
    const bool interrupted=settings_.value("scanInProgress",false).toBool();
    pausedScan_=settings_.value("resumeAvailable",false).toBool() || interrupted;
    lastScanElapsedMs_=std::max<qint64>(0,settings_.value("lastScanElapsedMs",0).toLongLong());
    lastScanOutcome_=settings_.value("lastScanOutcome").toString();
    if(!QStringList{"completed","stopped","paused","failed"}.contains(lastScanOutcome_)) lastScanOutcome_.clear();
    createActions(); createLayout(); translate(); applyPrivacy(); updateUndoAction(); updateMediaMode();
    for(auto panel:{left_,right_}) panel->fileMenuRequested=[this,panel](QPoint point) {
        if(fileOperationPending_ || scanning_ || closing_) return;
        QMenu menu(this); addTransferActions(&menu,panel->fileIndex); menu.exec(point);
    };
    for(auto button:findChildren<QToolButton*>()) button->setFocusPolicy(Qt::StrongFocus);
    profile_->setCurrentIndex(std::clamp(settings_.value("profile",0).toInt(),0,2));
    if(settings_.contains("geometry")) restoreGeometry(settings_.value("geometry").toByteArray());
    if(settings_.contains("splitter")) vertical_->restoreState(settings_.value("splitter").toByteArray());
    timer_.setInterval(33); connect(&timer_,&QTimer::timeout,this,[this] { tick(); }); timer_.start();
    reviewSaveTimer_.setInterval(2000); connect(&reviewSaveTimer_,&QTimer::timeout,this,[this] { saveLastReview(); }); reviewSaveTimer_.start();
    QTimer::singleShot(0,this,[this] {
        fitToScreen();
        if(auto handle=windowHandle()) connect(handle,&QWindow::screenChanged,this,[this] { QTimer::singleShot(0,this,[this] { fitToScreen(); }); });
        for(auto desktop:QGuiApplication::screens()) connect(desktop,&QScreen::availableGeometryChanged,this,[this] { QTimer::singleShot(0,this,[this] { fitToScreen(); }); });
    });
    if(interrupted) statusBar()->showMessage(text("The previous scan was interrupted. Resume paused scan is available in File."));
    else if(QFileInfo::exists(lastReviewPath_)) statusBar()->showMessage(text("Your last review is available in File > Resume last review."));
    if(settings_.value("cleanupInProgress",false).toBool()) statusBar()->showMessage(text("A file operation was interrupted. Recover the last cleanup review from File and inspect operation history. Nothing will be repeated automatically."));
}
MainWindow::~MainWindow() {
    bulkCancel_.store(true); if(batchTransferWatcher_) batchTransferWatcher_->waitForFinished();
    reviewSaveTimer_.stop(); if(reviewSaveCancel_) reviewSaveCancel_->store(true); if(reviewSaveWatcher_) reviewSaveWatcher_->waitForFinished();
    cancel_.store(true); if(alignmentWatcher_) alignmentWatcher_->waitForFinished();
    if(verificationWatcher_) verificationWatcher_->waitForFinished();
    if(audioEvidenceWatcher_) audioEvidenceWatcher_->waitForFinished();
    if(partialSearchWatcher_) partialSearchWatcher_->waitForFinished();
    if(clearCacheWatcher_) clearCacheWatcher_->waitForFinished();
    timer_.stop(); cancel_.store(true); if(scanThread_.joinable()) scanThread_.join();
    left_->player->Close(); right_->player->Close(); previewPool_.waitForDone(); saveSettings();
}
void MainWindow::fitToScreen() {
    auto desktop=screen(); if(!desktop) return;
    const auto available=desktop->availableGeometry();
    const auto frameExtra=frameGeometry().size()-size();
    const auto maximum=(available.size()-frameExtra-QSize(2,2)).expandedTo(minimumSizeHint());
    setMinimumSize(QSize(std::min(1040,maximum.width()),std::min(720,maximum.height())));
    if(isMaximized() || isFullScreen()) return;
    resize(size().boundedTo(maximum));
    const auto frame=frameGeometry();
    const QPoint target(std::clamp(frame.left(),available.left(),std::max(available.left(),available.right()-frame.width()+1)),
        std::clamp(frame.top(),available.top(),std::max(available.top(),available.bottom()-frame.height()+1)));
    move(pos()+target-frame.topLeft());
}

std::pair<int64_t,int64_t> MainWindow::comparisonRange() const {
    return {std::max<int64_t>(0,-alignmentOffset100ns_),
        std::min(left_->player->GetDuration100ns(),right_->player->GetDuration100ns()-alignmentOffset100ns_)};
}
void MainWindow::seekBoth(int64_t position,bool exact) {
    if(exact) reviewDirty_=true;
    bool resume=syncing_ || left_->player->IsPlaying() || right_->player->IsPlaying();
    left_->player->Pause(); right_->player->Pause();
    const auto [begin,end]=comparisonRange();
    if(end<=begin) { syncing_=false; statusBar()->showMessage(text("No overlapping playback range at this offset.")); return; }
    position=std::clamp(position,begin,end);
    if(exact) {
        left_->player->SeekTo(position); right_->player->SeekTo(position+alignmentOffset100ns_);
    } else {
        left_->player->ScrubTo(position); right_->player->ScrubTo(position+alignmentOffset100ns_);
    }
    syncPosition_=position; syncing_=resume;
    if(resume) syncTimer_.start();
}
void MainWindow::controlSeek(PlayerPanel* source,int64_t position,bool exact) {
    if(exact) reviewDirty_=true;
    lastControlledRight_=source==right_; linkedStepFromRight_.reset();
    if(linkAction_->isChecked()) seekBoth(lastControlledRight_?position-alignmentOffset100ns_:position,exact);
    else { syncing_=false; if(exact) source->player->SeekTo(position); else source->player->ScrubTo(position); }
}

void MainWindow::playBoth(PlayerPanel* source) {
    if(options_.images) return;
    if(left_->player->IsPlaying() || right_->player->IsPlaying()) { syncing_=false; left_->player->Pause(); right_->player->Pause(); return; }
    if(!left_->player->IsLoaded() || !right_->player->IsLoaded()) return;
    const auto [begin,end]=comparisonRange();
    if(end<=begin) { syncing_=false; statusBar()->showMessage(text("No overlapping playback range at this offset.")); return; }
    syncPosition_=source==right_?right_->player->GetPosition100ns()-alignmentOffset100ns_:left_->player->GetPosition100ns();
    if(left_->player->IsEof() || right_->player->IsEof() || syncPosition_<begin || syncPosition_>=end-500000) syncPosition_=begin;
    left_->player->SeekTo(syncPosition_); right_->player->SeekTo(syncPosition_+alignmentOffset100ns_); syncing_=true; syncTimer_.start();
}
void MainWindow::tick() {
    if(options_.images) {
        playBothAction_->setEnabled(false);
        linkAction_->setEnabled(left_->animationReady() && right_->animationReady());
        left_->tickImage();
        if(linkAction_->isChecked() && left_->animationReady() && right_->animationReady() && left_->imagePlaying()) right_->seekImage(left_->imageTimeline());
        else right_->tickImage();
    }
    updateReviewState();
    if(scanning_) updateScanTimeLabel();
    pauseAction_->setEnabled(scanning_); resumeAction_->setEnabled(pausedScan_ && !scanning_);
    left_->refresh(); right_->refresh();
    bool alignmentAvailable=!scanning_ && !fileOperationPending_ && left_->player->IsLoaded() && right_->player->IsLoaded();
    offset_->parentWidget()->setEnabled(alignmentAvailable);
    // Looked up once: a recursive findChild per tick walks every result row.
    if(!tickTargets_.resolved) {
        tickTargets_={findChild<QToolButton*>("resetAlignment"),findChild<QToolButton*>("autoAlignPartialClip"),findChild<QAction*>("Auto-align partial clip"),findChild<QAction*>("Compare sampled audio"),findChild<QAction*>("Mark right file for cleanup"),true};
    }
    if(tickTargets_.resetAlignment) tickTargets_.resetAlignment->setEnabled(alignmentAvailable && offset_->value()!=0);
    if(tickTargets_.autoAlign) tickTargets_.autoAlign->setEnabled(alignmentAvailable);
    if(tickTargets_.autoAlignAction) tickTargets_.autoAlignAction->setEnabled(alignmentAvailable);
    if(tickTargets_.compareAudio) tickTargets_.compareAudio->setEnabled(alignmentAvailable);
    if(options_.images) restorePositions_.reset();
    if(restorePositions_ && left_->player->IsLoaded() && right_->player->IsLoaded()) {
        left_->player->Pause(); right_->player->Pause(); left_->player->SeekTo(restorePositions_->first); right_->player->SeekTo(restorePositions_->second); restorePositions_.reset();
    }
    bool playing=left_->player->IsPlaying() || right_->player->IsPlaying(); setGlyph(playBothAction_,playing?Glyph::Pause:Glyph::Play);
    if(linkedStepFromRight_) {
        auto source=*linkedStepFromRight_?right_:left_; auto other=*linkedStepFromRight_?left_:right_;
        if(!source->player->IsSeeking()) {
            const auto [begin,end]=comparisonRange();
            const auto position=source->player->GetPosition100ns()-(*linkedStepFromRight_?alignmentOffset100ns_:0);
            if(end>begin && position>=begin && position<=end) other->player->SeekTo(position+(other==right_?alignmentOffset100ns_:0));
            else seekBoth(position,true);
            linkedStepFromRight_.reset();
        }
    }
    if(syncing_ && (FAILED(left_->player->GetError()) || FAILED(right_->player->GetError()) || syncTimer_.elapsed()>30000)) {
        syncing_=false; statusBar()->showMessage(text("Preview unavailable"));
    }
    if(syncing_ && !left_->isScrubbing() && !right_->isScrubbing() && left_->player->IsLoaded() && right_->player->IsLoaded() && !left_->player->IsSeeking() && !right_->player->IsSeeking()) {
        syncing_=false; auto start=std::chrono::steady_clock::now()+std::chrono::milliseconds(40);
        left_->player->PlayAt(start,syncPosition_); right_->player->PlayAt(start,std::clamp<int64_t>(syncPosition_+alignmentOffset100ns_,0,right_->player->GetDuration100ns()));
    }
    if(linkAction_->isChecked() && !syncing_ && !left_->player->IsSeeking() && !right_->player->IsSeeking() &&
        left_->player->IsPlaying()!=right_->player->IsPlaying()) {
        left_->player->Pause(); right_->player->Pause();
    }
}

void MainWindow::findPartialClips() {
    if(scanning_ || sortingMetadata_ || fileOperationPending_ || clearingCache_ || closing_) return;
    auto roots=folders_;
    if(roots.empty()) {
        const auto folder=QFileDialog::getExistingDirectory(this,text("Find partial clips in folders…"));
        if(folder.isEmpty()) return; roots.push_back(folder.toStdWString());
    }
    auto criteria=options_; criteria.images=false; criteria.extensions.clear(); criteria.matchDuration=false; criteria.matchSize=false;
    { ScanCache cache(cachePath_); criteria.ignoredPairs=cache.ignoredPairs(); }
    cancel_.store(false); sortingMetadata_=true; syncing_=false; left_->player->Pause(); right_->player->Pause();
    auto progress=new QProgressDialog(text("Deep partial-clip search; this can take longer than a similarity scan."),text("Cancel"),0,0,this);
    progress->setObjectName("partialSearchProgress"); progress->setWindowModality(Qt::WindowModal); progress->setMinimumDuration(0); progress->show();
    connect(progress,&QProgressDialog::canceled,this,[this] { cancel_.store(true); });
    auto watcher=new QFutureWatcher<PartialClipSearch::Result>(this); partialSearchWatcher_=watcher;
    connect(watcher,&QFutureWatcherBase::finished,this,[this,watcher,progress] {
        auto result=watcher->result(); watcher->deleteLater(); partialSearchWatcher_=nullptr; progress->hide(); progress->deleteLater(); sortingMetadata_=false;
        if(pendingClose_) { close(); return; }
        if(closing_ || result.cancelled || cancel_.load()) return;
        QDialog dialog(this); dialog.setObjectName("partialSearchResults"); dialog.setWindowTitle(text("Partial-clip candidates — not duplicate groups")); dialog.resize(760,430);
        auto layout=new QVBoxLayout(&dialog);
        auto summary=new QLabel(text("%1 candidates · %2 skipped · %3 failed · %4 ambiguous").arg(result.matches.size()).arg(result.skipped).arg(result.failed).arg(result.ambiguous),&dialog); summary->setWordWrap(true); layout->addWidget(summary);
        auto scope=new QLabel(text("Sampled candidates only; no full-copy claim. Opened pairs are protected from cleanup until you explicitly unlock them."),&dialog); scope->setWordWrap(true); layout->addWidget(scope);
        if(result.limited) { auto limit=new QLabel(text("Search limits or repetitive hash buckets were reached; these results are incomplete."),&dialog); limit->setWordWrap(true); layout->addWidget(limit); }
        auto table=new QTreeWidget(&dialog); table->setObjectName("partialCandidates"); table->setRootIsDecorated(false); table->setHeaderLabels({text("File name"),text("File name"),text("Overlap"),text("Offset"),text("Similarity")}); layout->addWidget(table);
        for(const auto& match:result.matches) {
            const auto& a=result.files[match.left]; const auto& b=result.files[match.right];
            auto row=new QTreeWidgetItem(table,{QString::fromStdWString(a.fileName),QString::fromStdWString(b.fileName),duration(match.duration),QString::number(double(match.offset)/10000000,'f',1)+" s",QString::number(match.similarity,'f',1)+"%"});
            row->setToolTip(0,QString::fromStdWString(a.fullPath)); row->setToolTip(1,QString::fromStdWString(b.fullPath));
        }
        table->header()->setSectionResizeMode(0,QHeaderView::Stretch); table->header()->setSectionResizeMode(1,QHeaderView::Stretch);
        table->header()->setStretchLastSection(false); for(int column:{2,3,4}) table->header()->setSectionResizeMode(column,QHeaderView::ResizeToContents);
        auto buttons=new QDialogButtonBox(QDialogButtonBox::Close,&dialog); auto open=buttons->addButton(text("Open pair for review"),QDialogButtonBox::ActionRole); open->setObjectName("openPartialPair"); open->setEnabled(!result.matches.empty()); layout->addWidget(buttons);
        if(table->topLevelItemCount()) table->setCurrentItem(table->topLevelItem(0));
        connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
        connect(open,&QPushButton::clicked,&dialog,[&] {
            const int index=table->indexOfTopLevelItem(table->currentItem()); if(index<0) return;
            const auto& match=result.matches[size_t(index)]; const auto& a=result.files[match.left]; const auto& b=result.files[match.right];
            if(FileOperations::ReadIdentity(a.fullPath)!=a.identity || FileOperations::ReadIdentity(b.fullPath)!=b.identity) { statusBar()->showMessage(text("This file changed or is no longer available. Rescan before removing it.")); return; }
            openFiles({QString::fromStdWString(a.fullPath),QString::fromStdWString(b.fullPath)});
            if(files_.size()!=2 || !manual_) return;
            for(auto& file:files_) file.protectedFile=true;
            left_->setFile(&files_[0],0); right_->setFile(&files_[1],1); updateReviewState();
            offset_->setValue(double(match.offset)/10000000);
            restorePositions_=std::make_pair(match.start,match.start+match.offset);
            statusBar()->showMessage(text("Sampled candidates only; no full-copy claim. Opened pairs are protected from cleanup until you explicitly unlock them."));
            dialog.accept();
        });
        dialog.exec();
    });
    watcher->setFuture(QtConcurrent::run([this,roots=std::move(roots),criteria,guard=QPointer<QProgressDialog>(progress)] {
        try { return PartialClipSearch::Find(roots,criteria,cancel_,[this,guard](const ScanProgress& p) {
            QMetaObject::invokeMethod(this,[guard,current=p.current,total=p.total] { if(guard) guard->setLabelText(text("Partial-clip timelines: %1 / %2").arg(current).arg(total)); },Qt::QueuedConnection);
        }); } catch(...) { PartialClipSearch::Result r; r.failed=1; r.limited=true; return r; }
    }));
}
void MainWindow::comparePairAudio() {
    if(scanning_ || sortingMetadata_ || fileOperationPending_ || clearingCache_ || closing_ || options_.images || left_->fileIndex<0 || right_->fileIndex<0) return;
    auto job=[](const VideoFileInfo& f) { VideoFileInfo v; v.fullPath=f.fullPath; v.identity=f.identity; v.duration100ns=f.duration100ns; return v; };
    const auto a=job(files_[left_->fileIndex]),b=job(files_[right_->fileIndex]); const auto generation=generation_;
    const auto offset=alignmentOffset100ns_;
    cancel_.store(false); sortingMetadata_=true; syncing_=false; left_->player->Pause(); right_->player->Pause();
    auto dialog=new QProgressDialog(text("Compare sampled audio"),text("Cancel"),0,0,this);
    dialog->setWindowModality(Qt::WindowModal); dialog->setMinimumDuration(0); dialog->show();
    connect(dialog,&QProgressDialog::canceled,this,[this] { cancel_.store(true); });
    auto watcher=new QFutureWatcher<AudioEvidence::Result>(this); audioEvidenceWatcher_=watcher;
    connect(watcher,&QFutureWatcherBase::finished,this,[this,watcher,dialog,a,b,generation] {
        const auto result=watcher->result(); watcher->deleteLater(); audioEvidenceWatcher_=nullptr;
        dialog->hide(); dialog->deleteLater(); sortingMetadata_=false;
        if(pendingClose_) { close(); return; }
        if(cancel_.load() || closing_ || generation!=generation_ || left_->fileIndex<0 || right_->fileIndex<0) return;
        const auto& l=files_[left_->fileIndex]; const auto& r=files_[right_->fileIndex];
        if(l.fullPath!=a.fullPath || r.fullPath!=b.fullPath || l.identity!=a.identity || r.identity!=b.identity || FileOperations::ReadIdentity(a.fullPath)!=a.identity || FileOperations::ReadIdentity(b.fullPath)!=b.identity) return;
        using S=AudioEvidence::Status;
        const char* key=result.status==S::SimilarSamples?"Audio samples are similar":result.status==S::DifferentSamples?"Audio samples differ or need alignment":result.status==S::NeitherHasAudio?"Neither file has an audio track":result.status==S::OneHasAudio?"Only one file has an audio track":result.status==S::Failed?"Audio comparison failed":"Audio evidence is inconclusive";
        QString message=text(key)+"\n\n"+text("Primary audio track only; three 6-second windows at the current offset. Silence, short clips and uncertain results do not establish equality. This does not authorize deletion.");
        message+="\n"+text("Audio tracks: %1 / %2 · completed windows: %3").arg(result.leftTracks).arg(result.rightTracks).arg(result.windows);
        statusBar()->showMessage(text(key));
        QMessageBox box(QMessageBox::Information,text("Compare sampled audio"),message,QMessageBox::Ok,this); box.setObjectName("audioEvidenceResult"); box.exec();
    });
    watcher->setFuture(QtConcurrent::run([this,a,b,offset] { try { return AudioEvidence::Compare(a,b,offset,cancel_); } catch(...) { return AudioEvidence::Result{}; } }));
}
void MainWindow::autoAlignPartialClip() {
    if(scanning_ || sortingMetadata_ || fileOperationPending_ || clearingCache_ || closing_ || options_.images || left_->fileIndex<0 || right_->fileIndex<0) return;
    const auto& a=files_[left_->fileIndex]; const auto& b=files_[right_->fileIndex];
    if(!a.identity.valid || !b.identity.valid || FileOperations::ReadIdentity(a.fullPath)!=a.identity || FileOperations::ReadIdentity(b.fullPath)!=b.identity) {
        statusBar()->showMessage(text("This file changed or is no longer available. Rescan before removing it.")); return;
    }
    const auto leftFile=a,rightFile=b;
    const auto generation=generation_;
    const double threshold=options_.minFrameSimilarity;
    cancel_.store(false); sortingMetadata_=true;
    left_->player->Pause(); right_->player->Pause(); syncing_=false;
    auto dialog=new QProgressDialog(text("Auto-align partial clip"),text("Cancel"),0,0,this);
    dialog->setObjectName("alignmentProgress"); dialog->setWindowModality(Qt::WindowModal);
    dialog->setMinimumDuration(0); dialog->setAutoClose(false); dialog->show();
    connect(dialog,&QProgressDialog::canceled,this,[this] { cancel_.store(true); });
    auto watcher=new QFutureWatcher<PartialClipAlignment>(this); alignmentWatcher_=watcher;
    connect(watcher,&QFutureWatcherBase::finished,this,[this,watcher,dialog,generation,leftFile,rightFile] {
        const auto result=watcher->result(); watcher->deleteLater(); alignmentWatcher_=nullptr;
        dialog->hide(); dialog->deleteLater(); sortingMetadata_=false;
        if(pendingClose_) { close(); return; }
        if(cancel_.load() || closing_ || generation!=generation_ || left_->fileIndex<0 || right_->fileIndex<0) return;
        const auto& currentLeft=files_[left_->fileIndex]; const auto& currentRight=files_[right_->fileIndex];
        if(currentLeft.fullPath!=leftFile.fullPath || currentRight.fullPath!=rightFile.fullPath ||
           currentLeft.identity!=leftFile.identity || currentRight.identity!=rightFile.identity ||
           FileOperations::ReadIdentity(leftFile.fullPath)!=leftFile.identity || FileOperations::ReadIdentity(rightFile.fullPath)!=rightFile.identity) return;
        if(result.status==PartialClipStatus::PartialOverlap || result.status==PartialClipStatus::FullTimelineMatch) {
            const double offsetSec=static_cast<double>(result.offset100ns)/10000000.0;
            offset_->setValue(offsetSec);
            seekBoth(std::max<int64_t>(0,result.overlapStartLeft100ns),true);
            const QString msg=text("Partial clip match: %1% (%2 s overlap, offset %3 s — review manually; not a full copy)")
                .arg(result.similarity,0,'f',1)
                .arg(static_cast<double>(result.overlapDuration100ns)/10000000.0,0,'f',1)
                .arg(offsetSec,0,'f',2);
            partialVerdict_=msg; partialVerdictKind_="similar"; partialPair_={left_->fileIndex,right_->fileIndex}; setVerdict(msg,"similar");
            statusBar()->showMessage(msg);
        } else {
            const QString reason=result.status==PartialClipStatus::Ambiguous
                ? text("Partial clip alignment: ambiguous or repetitive content (no offset applied)")
                : text("Partial clip alignment: not found / insufficient visual evidence");
            // The result stays on this pair like a match does, instead of giving way to the next preview refresh.
            partialVerdict_=reason; partialVerdictKind_="none"; partialPair_={left_->fileIndex,right_->fileIndex}; setVerdict(reason,"none");
            statusBar()->showMessage(reason);
        }
    });
    watcher->setFuture(QtConcurrent::run([this,leftFile,rightFile,threshold] {
        try { return VideoScanner::EstimatePartialClipAlignment(leftFile.fullPath,rightFile.fullPath,&cancel_,threshold); }
        catch(...) { return PartialClipAlignment{}; }
    }));
}

void MainWindow::openTwoFiles() {
    if(scanning_) return;
    QString filter="Media (*.mp4 *.mkv *.mov *.avi *.webm *.wmv *.m4v *.flv *.mpg *.mpeg *.ts *.mts *.m2ts *.3gp *.vob *.ogv *.divx *.asf *.jpg *.jpeg *.jpe *.jfif *.png *.apng *.webp *.heic *.heif *.hif *.avif *.avifs *.bmp *.dib *.gif *.tif *.tiff *.tga *.icb *.vda *.vst *.ico *.cur *.icns *.pbm *.pgm *.ppm *.pnm *.pam *.pfm *.phm *.xbm *.xpm *.wbmp *.svg *.svgz *.psd *.psb *.jxl *.jp2 *.j2k *.jpc *.jpf *.jpx *.j2c *.jls *.jxr *.wdp *.hdp *.exr *.hdr *.pic *.dds *.pcx *.qoi *.sgi *.rgb *.rgba *.bw *.int *.inta *.ras *.sun *.dpx *.iff *.ilbm *.lbm *.dng *.cr2 *.cr3 *.nef *.nrw *.arw *.srf *.sr2 *.raf *.orf *.rw2 *.pef *.srw *.x3f *.3fr *.fff *.iiq *.erf *.kdc *.dcr *.mrw *.mos *.raw *.rwl);;All files (*)";
#ifndef _WIN32
    filter.remove(" *.jxr *.wdp *.hdp"); // JPEG XR is decoded only by Windows' WIC
#endif
    auto selection=QFileDialog::getOpenFileNames(this,text("Open two files…"),{},filter);
    openFiles(selection);
}
void MainWindow::openFiles(const QStringList& selection) {
    if(selection.isEmpty() || scanning_ || sortingMetadata_ || fileOperationPending_ || clearingCache_ || closing_) return;
    if(selection.size()!=2) { QMessageBox::information(this,text("Open two files…"),text("Choose exactly two files.")); return; }
    const bool image=StillImages::supported(selection[0].toStdWString());
    if(image!=StillImages::supported(selection[1].toStdWString())) { QMessageBox::information(this,text("Open two files…"),text("Choose two images or two videos.")); return; }
    if(options_.images!=image) mediaMode_->setCurrentIndex(image?1:0);
    ++generation_; pendingPreviews_.clear(); previewRuns_.clear(); requestedSamples_.clear(); markedFiles_.clear(); files_.clear(); groups_.clear(); manual_=true; currentGroup_=-1;
    for(auto path:selection) {
        VideoFileInfo file; QFileInfo info(path); file.fullPath=QDir::toNativeSeparators(path).toStdWString(); file.fileName=info.fileName().toStdWString(); file.folderPath=QDir::toNativeSeparators(info.absolutePath()).toStdWString(); file.fileSize=info.size(); file.identity=FileOperations::ReadIdentity(file.fullPath);
        for(const auto& root:options_.protectedFolders)
            if(VideoScanner::IsWithinFolder(file.fullPath,root)) file.protectedFile=file.protectedByFolder=true;
        files_.push_back(std::move(file));
    }
    if(files_[0].identity.valid && files_[0].identity==files_[1].identity) {
        files_.clear(); left_->setFile(nullptr,-1); right_->setFile(nullptr,-1); populateResults();
        QMessageBox::information(this,text("Open two files…"),text("Choose two different physical files.")); return;
    }
    VideoGroup group; group.fileIndices={0,1}; group.manualReference=true; groups_.push_back(group); populateResults(); selectGroup(0);
}
void MainWindow::saveSettings() {
    settings_.setValue("images",options_.images);
    settings_.setValue("resumeAvailable",pausedScan_);
    settings_.setValue("language",Language::selected()); settings_.setValue("folders",strings(folders_)); settings_.setValue("excluded",strings(options_.excludedFolders)); settings_.setValue("protected",strings(options_.protectedFolders));
    settings_.setValue("nonRecursiveFolders",strings(options_.nonRecursiveFolders));
    settings_.setValue("recursive",options_.includeSubfolders); settings_.setValue("workers",options_.workerCount); settings_.setValue("geometry",saveGeometry()); settings_.setValue("splitter",vertical_->saveState()); settings_.sync();
    settings_.setValue("minSize",qulonglong(options_.minFileSize)); settings_.setValue("maxSize",qulonglong(options_.maxFileSize)); settings_.setValue(options_.images?"imageExtensions":"extensions",strings(options_.extensions));
    settings_.setValue(options_.images?"imageExcludedExtensions":"excludedExtensions",strings(options_.excludedExtensions));
    settings_.setValue("similarity",options_.minFrameSimilarity); settings_.setValue("durationTolerance",qlonglong(options_.durationTol100ns));
    settings_.setValue("matchSize",options_.matchSize); settings_.setValue("matchDuration",options_.matchDuration); settings_.setValue("matchResolution",options_.matchResolution); settings_.setValue("matchFirstFrame",options_.matchFirstFrame);
    settings_.setValue("matchFileName",options_.matchFileName); settings_.setValue("matchExtension",options_.matchExtension);
    settings_.setValue("visualTransforms",options_.matchTransforms); settings_.setValue("crossFolderOnly",options_.crossFolderOnly);
    settings_.setValue("sort",int(options_.sortOrder)); settings_.setValue("profile",profile_->currentIndex()); settings_.sync();
}
void MainWindow::closeEvent(QCloseEvent* event) {
    if(fileOperationPending_) { bulkCancel_.store(true); pendingClose_=true; event->ignore(); statusBar()->showMessage(text("Stopping…")); return; }
    if(scanning_) { pendingClose_=true; pausedScan_=true; cancel_.store(true); saveSettings(); statusBar()->showMessage(text("Stopping…")); event->ignore(); return; }
    if(sortingMetadata_) { cancel_.store(true); pendingClose_=true; event->ignore(); statusBar()->showMessage(text("Stopping…")); return; }
    if(clearingCache_) { pendingClose_=true; event->ignore(); statusBar()->showMessage(text("Stopping…")); return; }
    if(closeReady_) { saveSettings(); event->accept(); return; }
    if(!closing_ && !reviewCloseAttempted_) {
        syncing_=false; left_->player->Pause(); right_->player->Pause();
        if(reviewSaving_ || (reviewCloseAttempted_=saveLastReview(true))) {
            pendingClose_=true; centralWidget()->setEnabled(false); menuBar()->setEnabled(false);
            statusBar()->showMessage(text("Saving last review…")); event->ignore(); return;
        }
    }
    event->ignore();
    if(closing_) return;
    closing_=true; syncing_=false; cancel_.store(true); ++generation_; timer_.stop();
    centralWidget()->setEnabled(false); menuBar()->setEnabled(false);
    // Keep widgets alive until background readers finish, without blocking the event loop.
    left_->canvas->hide(); right_->canvas->hide();
    left_->player->RequestStop(); right_->player->RequestStop();
    statusBar()->showMessage(text("Closing media readers…"));
    auto watcher=new QFutureWatcher<void>(this);
    connect(watcher,&QFutureWatcher<void>::finished,this,[this,watcher] {
        watcher->deleteLater(); closeReady_=true; close();
    });
    watcher->setFuture(QtConcurrent::run([this] {
        left_->player->Close(); right_->player->Close(); previewPool_.waitForDone();
    }));
}
bool MainWindow::eventFilter(QObject* object,QEvent* event) {
    // The verdict chip ends in "…" when the row is too narrow; the full verdict stays in its tooltip.
    if(object==similarity_ && event->type()==QEvent::Resize) {
        // The full text is kept apart: the tooltip may be rewrapped as rich text. contentsRect already excludes the padding.
        const auto full=similarity_->property("fullText").toString(); const int room=similarity_->contentsRect().width();
        const auto shown=similarity_->fontMetrics().horizontalAdvance(full)<=room?full:similarity_->fontMetrics().elidedText(full,Qt::ElideRight,std::max(0,room));
        if(!full.isEmpty() && similarity_->text()!=shown) similarity_->setText(shown);
    }
    if((event->type()==QEvent::Move || event->type()==QEvent::Resize || event->type()==QEvent::Show) && (object==left_ || object->objectName()=="playbackBridge" || (object->objectName()=="playerSeek" && left_->isAncestorOf(static_cast<QWidget*>(object))))) {
        auto bridge=findChild<QWidget*>("playbackBridge"); auto link=findChild<QToolButton*>("linkControls"); auto seek=left_->findChild<QSlider*>("playerSeek");
        if(bridge && link && seek) link->move((bridge->width()-link->width())/2,seek->mapTo(bridge->parentWidget(),seek->rect().center()).y()-bridge->y()-link->height()/2);
    }
    if(!results_) return QMainWindow::eventFilter(object,event);
    // The keyboard menu key reaches the list itself; everything else arrives on its viewport.
    if(object==results_->viewport() || (object==results_ && event->type()==QEvent::ContextMenu)) {
        auto groupAt=[this](QPoint position) { auto item=results_->itemAt(position); return item?item->data(0,Qt::UserRole).toInt():-1; };
        switch(event->type()) {
        case QEvent::MouseButtonPress: case QEvent::MouseButtonDblClick: {
            auto mouse=static_cast<QMouseEvent*>(event); if(mouse->button()!=Qt::LeftButton) break;
            const auto position=mouse->position().toPoint(); const int group=groupAt(position); if(group<0) break;
            const int slot=thumbnailAt(group,position);
            // Scrollers move the row by three thumbnails and never change the comparison.
            if(slot==-2 || slot==-3) { setRowScroll(group,rowScroll(group)+(slot==-2?-3:3)*(ResultRow::cell+ResultRow::spacing)); return true; }
            selectGroup(group,slot>0?slot:-1);
            break;
        }
        case QEvent::MouseMove: case QEvent::Leave: {
            const auto position=event->type()==QEvent::Leave?QPoint(-1,-1):static_cast<QMouseEvent*>(event)->position().toPoint();
            const int group=event->type()==QEvent::Leave?-1:groupAt(position); const int slot=group<0?-1:thumbnailAt(group,position);
            if(group!=hoveredGroup_ || slot!=hoveredSlot_) {
                for(int changed:{hoveredGroup_,group}) if(auto item=groupItem(changed)) results_->viewport()->update(results_->visualItemRect(item));
                hoveredGroup_=group; hoveredSlot_=slot;
            }
            break;
        }
        case QEvent::Wheel: {
            // Shift+wheel or a sideways wheel scrolls the row under the pointer; a plain wheel scrolls the list.
            auto wheel=static_cast<QWheelEvent*>(event); const int group=groupAt(wheel->position().toPoint());
            const int delta=wheel->angleDelta().x()?wheel->angleDelta().x():(wheel->modifiers()&Qt::ShiftModifier)?wheel->angleDelta().y():0;
            if(group>=0 && delta && rowScrollMax(group)>0) { setRowScroll(group,rowScroll(group)-delta*(ResultRow::cell+ResultRow::spacing)/120); return true; }
            break;
        }
        case QEvent::ContextMenu: {
            auto menuEvent=static_cast<QContextMenuEvent*>(event);
            int group=groupAt(menuEvent->pos()),slot=group>=0?thumbnailAt(group,menuEvent->pos()):-1;
            if(menuEvent->reason()==QContextMenuEvent::Keyboard && currentGroup_>=0) { group=currentGroup_; slot=groups_[group].rightSlot; }
            if(group>=0 && slot>=0) {
                const auto global=menuEvent->globalPos(); const auto generation=generation_;
                QTimer::singleShot(0,this,[this,group,slot,global,generation] { if(generation==generation_) showThumbnailMenu(group,slot,global); });
                return true;
            }
            break;
        }
        default: break;
        }
    }
    bool resultControl=object==results_ || object==results_->viewport();
    if(resultControl && event->type()==QEvent::KeyPress) {
        auto key=static_cast<QKeyEvent*>(event);
        if(key->modifiers()==Qt::NoModifier && !scanning_ && !sortingMetadata_ && !fileOperationPending_ && currentGroup_>=0) {
            int group=currentGroup_,slot=groups_[group].rightSlot; bool handled=true;
            switch(key->key()) {
            case Qt::Key_Up: case Qt::Key_Down: {
                int step=key->key()==Qt::Key_Up?-1:1, next=group+step;
                while(next>=0 && next<static_cast<int>(groups_.size()) && results_->topLevelItem(next)->isHidden()) next+=step;
                if(next>=0 && next<static_cast<int>(groups_.size())) { group=next; slot=groups_[group].rightSlot; }
                break;
            }
            case Qt::Key_Left: slot=std::max(1,slot-1); break;
            case Qt::Key_Right: slot=std::min(static_cast<int>(groups_[group].fileIndices.size())-1,slot+1); break;
            case Qt::Key_Home: slot=1; break;
            case Qt::Key_End: slot=static_cast<int>(groups_[group].fileIndices.size())-1; break;
            default: handled=false;
            }
            if(handled) { selectGroup(group,slot); key->accept(); return true; }
        }
    }
    if(object==results_->viewport() && event->type()==QEvent::Resize)
        QTimer::singleShot(0,this,[this] { requestVisiblePreviews(); });
    return QMainWindow::eventFilter(object,event);
}
void MainWindow::dragEnterEvent(QDragEnterEvent* event) { if(event->mimeData()->hasUrls() && !scanning_) event->acceptProposedAction(); }
void MainWindow::dropEvent(QDropEvent* event) {
    QStringList paths; for(const auto& url:event->mimeData()->urls()) if(url.isLocalFile()) paths<<url.toLocalFile();
    if(paths.isEmpty()) return;
    event->acceptProposedAction();
    // Two files open as a pair, and an empty source list has nothing to lose: both act at once.
    const bool pair=paths.size()==2 && QFileInfo(paths[0]).isFile() && QFileInfo(paths[1]).isFile();
    if(pair || folders_.empty()) { QTimer::singleShot(0,this,[this,paths] { openPaths(paths); }); return; }
    // Existing sources may carry protection and subfolder choices, so ask whether the drop replaces or extends them.
    // The menu opens after the drop has finished, so the program the items came from is not kept waiting.
    const auto position=mapToGlobal(event->position().toPoint());
    QTimer::singleShot(0,this,[this,paths,position] {
        QMenu menu(this); menu.setObjectName("dropChoiceMenu");
        auto only=menu.addAction(text("Scan only these")); auto add=menu.addAction(text("Add to sources and scan"));
        menu.addSeparator(); menu.addAction(text("Cancel"));
        menu.setDefaultAction(only); menu.setActiveAction(only);
        const auto chosen=menu.exec(position);
        if(chosen==only) openPaths(paths); else if(chosen==add) openPaths(paths,true);
    });
}
// One entry point for drag and drop, Send To and the command line: two files are compared side by side;
// anything else (folders, one file with folders, three or more files) is scanned, either alone or added to the current sources.
bool MainWindow::openPaths(const QStringList& handed,bool addToSources) {
    // Dropped or opened items may arrive as sandbox aliases; sources and files are kept under their real paths.
    QStringList paths; for(const auto& path:handed) paths<<QString::fromStdWString(Platform::HostPath(QDir::cleanPath(path).toStdWString()));
    if(paths.isEmpty()) return false;
    if(scanning_ || sortingMetadata_ || fileOperationPending_ || clearingCache_ || closing_) { statusBar()->showMessage(text("Wait for the current operation to finish.")); return false; }
    QStringList files; std::vector<std::wstring> sources;
    for(const auto& path:paths) {
        const QFileInfo info(path); if(!info.exists()) continue;
        const auto native=QDir::toNativeSeparators(info.absoluteFilePath()).toStdWString();
        if(std::find(sources.begin(),sources.end(),native)!=sources.end()) continue;
        if(info.isFile()) files<<info.absoluteFilePath(); else if(!info.isDir()) continue;
        sources.push_back(native);
    }
    if(sources.empty()) return false;
    if(files.size()==2 && sources.size()==2) { openFiles(files); return true; }
    if(files.size()==1 && sources.size()==1) {
        QMessageBox::information(this,text("Open two files…"),text("Drop or send two files to compare them, or a folder to scan it.")); return false;
    }
    if(addToSources) {
        for(auto& source:sources) if(std::find(folders_.begin(),folders_.end(),source)==folders_.end()) folders_.push_back(std::move(source));
    } else {
        // Only files decide the media type, and only when nothing else is scanned with them.
        bool allImages=!files.isEmpty(),allVideos=!files.isEmpty();
        for(const auto& file:files) { const bool image=StillImages::supported(file.toStdWString()); allImages&=image; allVideos&=!image; }
        if(allImages && sources.size()==size_t(files.size()) && !options_.images) mediaMode_->setCurrentIndex(1);
        else if(allVideos && sources.size()==size_t(files.size()) && options_.images) mediaMode_->setCurrentIndex(0);
        folders_=std::move(sources);
    }
    updateSourceSummary(); saveSettings();
    statusBar()->showMessage(text(addToSources?"Added to sources; scanning.":"Scanning the dropped items."));
    startScan(); return true;
}
