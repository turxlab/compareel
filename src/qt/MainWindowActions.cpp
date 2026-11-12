#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "Language.h"
#include "Icons.h"
#include "Theme.h"
#include "ScanCache.h"
#include "FFmpegReader.h"
#include <QActionGroup>
#include <QApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QStatusBar>
#include <QUrl>
#include <QEventLoop>
#include <QFutureWatcher>
#include <QPointer>
#include <QtConcurrent/QtConcurrentRun>

using Language::text;

QAction* MainWindow::action(const char* key,const QKeySequence& shortcut,std::function<void()> callback) {
    auto item=new QAction(this); item->setObjectName(QString::fromUtf8(key)); item->setShortcut(shortcut);
    connect(item,&QAction::triggered,this,std::move(callback)); actionTexts_.push_back({item,key}); return item;
}
void MainWindow::createActions() {
    // Choosing sources and scanning are separate: a new scan of the same sources should not reopen the source list.
    foldersAction_=action("Scan sources…",QKeySequence::Open,[this] { chooseFolders(); }); foldersAction_->setIcon(icon(Glyph::Folder));
    // Scan is only ever shown on accent buttons; in menus its icon would vanish on a light theme.
    rescanAction_=action("Scan",QKeySequence(Qt::Key_F5),[this] { startScan(); }); rescanAction_->setIcon(icon(Glyph::Search,"@onAccent")); rescanAction_->setIconVisibleInMenu(false);
    stopAction_=action("Stop scan",QKeySequence(Qt::Key_Escape),[this] { pausedScan_=false; scanPausable_=false; pauseAction_->setEnabled(false); cancel_.store(true); statusBar()->showMessage(text("Stopping…")); }); stopAction_->setIcon(icon(Glyph::Stop)); stopAction_->setEnabled(false);
    pauseAction_=action("Pause scan",QKeySequence("Ctrl+Shift+P"),[this] { if(!scanning_ || !scanPausable_) return; pausedScan_=true; scanPausable_=false; pauseAction_->setEnabled(false); cancel_.store(true); saveSettings(); statusBar()->showMessage(text("Stopping…")); }); pauseAction_->setIcon(icon(Glyph::Pause));
    resumeAction_=action("Resume paused scan",QKeySequence(),[this] { if(pausedScan_ && !scanning_) startScan(true); });
    pauseAction_->setEnabled(false); resumeAction_->setEnabled(pausedScan_);
    playBothAction_=action("Play / pause both",QKeySequence(Qt::Key_Space),[this] { playBoth(); }); playBothAction_->setIcon(icon(Glyph::Play));
    previousAction_=action("Previous group",QKeySequence(Qt::ALT|Qt::Key_Left),[this] { if(const int prev=adjacentVisibleGroup(currentGroup_,-1); prev>=0) selectGroup(prev); }); previousAction_->setIcon(icon(Glyph::PreviousGroup));
    nextAction_=action("Next group",QKeySequence(Qt::ALT|Qt::Key_Right),[this] { if(const int next=adjacentVisibleGroup(currentGroup_,1); next>=0) selectGroup(next); }); nextAction_->setIcon(icon(Glyph::NextGroup));
    linkAction_=action("Link video controls",QKeySequence("Ctrl+L"),[this] {
        linkedStepFromRight_.reset();
        if(linkAction_->isChecked() && left_->player->IsLoaded() && right_->player->IsLoaded())
            seekBoth(lastControlledRight_?right_->player->GetPosition100ns()-alignmentOffset100ns_:left_->player->GetPosition100ns(),true);
        else syncing_=false;
        linkAction_->setIcon(icon(linkAction_->isChecked()?Glyph::Link:Glyph::Unlink));
    });
    linkAction_->setCheckable(true); linkAction_->setChecked(true); linkAction_->setIcon(icon(Glyph::Link));
    connect(linkAction_,&QAction::toggled,this,[this](bool linked) {
        linkAction_->setIcon(icon(linked?Glyph::Link:Glyph::Unlink));
        linkAction_->setToolTip(nativeShortcuts(text(linked?"Controls linked · Ctrl+L to unlink":"Independent controls · Ctrl+L to link")));
    });
    reportAction_=action("Scan report",QKeySequence(),[this] { showScanReport(); });
    // Menus follow the review workflow: sources and sessions, per-pair review, playback, analysis tools, view, settings.
    auto file=menuBar()->addMenu(text("File")); file->setObjectName("File");
    file->addAction(foldersAction_); file->addAction(action("Open two files…",QKeySequence("Ctrl+Shift+O"),[this] { openTwoFiles(); }));
    file->addSeparator(); file->addAction(action("Save review session…",QKeySequence::Save,[this] { saveSession(); }));
    file->addAction(action("Open review session…",QKeySequence("Ctrl+Shift+L"),[this] { loadSession(); }));
    resumeReviewAction_=action("Resume last review",{},[this] {
        if(!loadSessionFile(lastReviewPath_)) QMessageBox::warning(this,text("Resume last review"),text(legacySessionNeedsScan_?"This older session has no saved results. Use Open review session to choose a one-time rebuild. Your current review is kept.":"The saved review could not be opened. Your current review is kept."));
    });
    resumeReviewAction_->setEnabled(QFileInfo::exists(lastReviewPath_)); file->addAction(resumeReviewAction_);
    file->addAction(action("Close results",QKeySequence(),[this] {
        if(reviewSaving_ || scanning_ || sortingMetadata_ || clearingCache_ || fileOperationPending_ || closing_) { statusBar()->showMessage(text("Wait for the current operation to finish.")); return; }
        clearResults(); statusBar()->showMessage(text("Ready"));
    }));
    file->addAction(action("Clear history…",QKeySequence(),[this] { showClearHistory(); }));
    forgetReviewAction_=action("Forget saved last review…",{},[this] {
        if(reviewSaving_ || scanning_ || sortingMetadata_ || clearingCache_ || fileOperationPending_ || closing_) { statusBar()->showMessage(text("Wait for the current operation to finish.")); return; }
        if(QMessageBox::question(this,text("Forget saved last review…"),text("Remove the saved last review and stop remembering it? Media, review decisions and operation history are kept."),QMessageBox::Yes|QMessageBox::No,QMessageBox::No)!=QMessageBox::Yes) return;
        if(QFileInfo::exists(lastReviewPath_) && !QFile::remove(lastReviewPath_)) { QMessageBox::warning(this,text("Forget saved last review…"),text("The saved review could not be removed.")); return; }
        rememberReviewAction_->setChecked(false); resumeReviewAction_->setEnabled(false); updateEmptyState();
    });
    recoverCleanupAction_=action("Recover last cleanup review…",QKeySequence(),[this] {
        if(!loadSessionFile(cleanupReviewPath_)) QMessageBox::warning(this,text("Recover last cleanup review…"),text(legacySessionNeedsScan_?"This older session has no saved results. Use Open review session to choose a one-time rebuild. Your current review is kept.":"The saved cleanup review could not be opened."));
        else finishCleanupRecovery();
    });
    recoverCleanupAction_->setEnabled(QFileInfo::exists(cleanupReviewPath_)); file->addAction(recoverCleanupAction_);
    file->addSeparator();
    file->addAction(action("Export results…",QKeySequence(),[this] { exportResults(); }));
    auto allFiles=file->addMenu(text("All result files")); allFiles->setObjectName("All result files");
    allFiles->addAction(action("Copy all result files…",{},[this] { chooseBatchTransfer(false); }));
    allFiles->addAction(action("Move all result files…",{},[this] { chooseBatchTransfer(true); }));
    file->addSeparator();
    for(bool right:{false,true}) {
        auto menu=file->addMenu(text(right?"Right file":"Left file")); menu->setObjectName(right?"Right file":"Left file");
        connect(menu,&QMenu::aboutToShow,this,[this,menu,right] { menu->clear(); addTransferActions(menu,(right?right_:left_)->fileIndex); });
    }
    undoTransferAction_=action("Undo last copy or move…",QKeySequence::Undo,[this] { undoLastTransfer(); });
    file->addAction(undoTransferAction_);
    file->addSeparator(); file->addAction(rescanAction_); file->addAction(pauseAction_); file->addAction(resumeAction_); file->addAction(stopAction_); file->addSeparator(); file->addAction(action("Exit",QKeySequence::Quit,[this] { close(); }));

    auto review=menuBar()->addMenu(text("Review")); review->setObjectName("Review");
    review->addAction(previousAction_); review->addAction(nextAction_);
    review->addAction(action("Next pair",QKeySequence(Qt::ALT|Qt::Key_Down),[this] { advanceToNextPair(); }));
    review->addSeparator();
    auto decisions=review->addMenu(text("Review decision")); decisions->setObjectName("Review decision");
    // "Unreviewed" is the absence of a decision, not a choice: pressing the active decision again clears it.
    for(int i=1;i<4;++i) {
        auto item=action(reviewLabels[i],QKeySequence(QString("Ctrl+%1").arg(i)),[this,i] {
            setReviewDecision(currentReviewDecision()==static_cast<ReviewDecision>(i)?ReviewDecision::Unreviewed:static_cast<ReviewDecision>(i));
        });
        item->setCheckable(true); item->setEnabled(false); decisions->addAction(item); reviewActions_[i]=item;
    }
    reviewActions_[0]=action("Clear review decision",QKeySequence(),[this] { setReviewDecision(ReviewDecision::Unreviewed); });
    reviewActions_[0]->setEnabled(false); decisions->addAction(reviewActions_[0]);
    decisions->addSeparator(); decisions->addAction(action("Reset all review decisions…",{},[this] {
        if(clearingCache_) return;
        if(scanning_ || sortingMetadata_ || fileOperationPending_ || closing_) return;
        if(QMessageBox::question(this,text("Reset all review decisions…"),text("Remove all review notes and their keep/later protection, including pairs outside this scan? Ignored pairs, file/folder locks and operation history remain. No files will be moved."),QMessageBox::Yes|QMessageBox::No,QMessageBox::No)!=QMessageBox::Yes) return;
        ScanCache cache(cachePath_); cache.clearReviewDecisions();
        if(!cache.error().isEmpty()) { plainWarning(this,text("Review decision"),cache.error()); return; }
        refreshReviewDecisions(); populateResults(); updateReviewState();
    }));
    review->addAction(action("Ignore this pair",QKeySequence("Ctrl+I"),[this] { ignoreCurrentPair(); }));
    review->addAction(action("Reset ignored pairs",QKeySequence(),[this] {
        if(sortingMetadata_ || fileOperationPending_ || clearingCache_ || closing_) return;
        if(scanning_) return;
        ScanCache cache(cachePath_); cache.clearIgnoredPairs();
        if(!cache.error().isEmpty()) { plainWarning(this,text("Scan report"),cache.error()); return; }
        scanCriteria_.ignoredPairs.clear();
        statusBar()->showMessage(text("Ignored pairs were reset. Scan again to show them in the results."));
    }));
    review->addSeparator();
    auto markRight=action("Mark right file for cleanup",QKeySequence("Ctrl+M"),[this] { toggleRightMark(); });
    markRight->setCheckable(true); markRight->setEnabled(false); review->addAction(markRight);
    review->addAction(action("Preview cleanup marks…",QKeySequence(),[this] { previewCleanupMarks(); }));
    review->addAction(action("Move marked files to Recycle Bin…",QKeySequence(),[this] { removeMarkedFiles(); }));
    review->addAction(action("Clear cleanup marks",QKeySequence(),[this] { markedFiles_.clear(); populateResults(); }));
    review->addSeparator();
    auto bulk=action("Move other copies to Recycle Bin…",{},[this] { previewBulkCleanup(); });
    bulk->setIcon(icon(Glyph::Trash)); review->addAction(bulk);
    review->addAction(action("Move focused file to Recycle Bin…",QKeySequence(Qt::Key_Delete),[this] { deleteFocusedFile(); })); findChild<QAction*>("Move focused file to Recycle Bin…")->setShortcuts(deleteShortcuts());

    auto playback=menuBar()->addMenu(text("Playback")); playback->setObjectName("Playback"); playback->addAction(playBothAction_);
    playback->addAction(action("Restart both",QKeySequence("Ctrl+Home"),[this] { syncing_=false; left_->player->Pause(); right_->player->Pause(); seekBoth(0,true); }));
    playback->addAction(linkAction_);
    playback->addAction(action("Auto-align partial clip",QKeySequence("Ctrl+Shift+A"),[this] { autoAlignPartialClip(); }));
    playback->addSeparator();
    auto modes=playback->addMenu(text("Comparison view")); modes->setObjectName("Comparison view"); auto exclusive=new QActionGroup(this);
    int mode=0;
    for(auto label:{"Side by side","Wipe","Overlay","Difference (display aid)"}) {
        int selected=mode++;
        auto item=action(label,QKeySequence(),[this,selected] { right_->canvas->setComparison(left_->canvas,selected); });
        item->setCheckable(true); item->setChecked(selected==0); exclusive->addAction(item); modes->addAction(item);
    }
    playback->addAction(action("Fit to window",QKeySequence("Ctrl+0"),[this] { left_->canvas->setTransform(1,{}); right_->canvas->setTransform(1,{}); }));

    auto tools=menuBar()->addMenu(text("Tools")); tools->setObjectName("Tools");
    tools->addAction(action("Verify exact matches in results",QKeySequence(),[this] { verifyResultHashes(); }));
    tools->addAction(action("Exact-copy groups first",QKeySequence(),[this] { sortVerifiedGroups(); }));
    tools->addAction(action("Compare sampled audio",QKeySequence(),[this] { comparePairAudio(); }));
    tools->addAction(action("Find partial clips in folders…",QKeySequence(),[this] { findPartialClips(); }));
    tools->addSeparator(); tools->addAction(reportAction_);
    tools->addAction(action("Operation history",QKeySequence(),[this] { showOperationHistory(); }));
    tools->addSeparator();
    tools->addAction(action("Scan rules…",QKeySequence("Ctrl+Shift+,"),[this] { advancedSettings(); }));
    tools->addAction(action("Settings…",QKeySequence("Ctrl+,"),[this] { showSettings(); }));

    auto view=menuBar()->addMenu(text("View")); view->setObjectName("View");
    privacyAction_=action("Hide all previews",QKeySequence("Ctrl+Shift+H"),[this] {
        { QSignalBlocker a(hideVideosAction_),b(hideThumbnailsAction_);
          hideVideosAction_->setChecked(privacyAction_->isChecked()); hideThumbnailsAction_->setChecked(privacyAction_->isChecked()); }
        applyPrivacy();
        if(privacyAction_->isChecked() && rememberReviewAction_->isChecked()) statusBar()->showMessage(text("While all previews are hidden, the last review is not saved."));
    });
    hideVideosAction_=action("Hide both video previews",QKeySequence(),[this] { applyPrivacy(); });
    hideThumbnailsAction_=action("Hide thumbnails",QKeySequence(),[this] { applyPrivacy(); });
    for(auto item:{privacyAction_,hideVideosAction_,hideThumbnailsAction_}) { item->setCheckable(true); view->addAction(item); }
    hideVideosAction_->setChecked(settings_.value("hideVideoPreviews",false).toBool());
    hideThumbnailsAction_->setChecked(settings_.value("hideThumbnails",false).toBool());
    view->addSeparator();
    auto onTop=action("Always on top",QKeySequence(),[this] {
        const bool enabled=findChild<QAction*>("Always on top")->isChecked(); const auto state=windowState();
        // Changing a window flag re-creates the native window, which hides it until shown again.
        setWindowFlag(Qt::WindowStaysOnTopHint,enabled); setWindowState(state); show();
        settings_.setValue("alwaysOnTop",enabled); settings_.sync();
    });
    onTop->setCheckable(true); onTop->setChecked(settings_.value("alwaysOnTop",false).toBool()); view->addAction(onTop);
    if(onTop->isChecked()) setWindowFlag(Qt::WindowStaysOnTopHint,true);

    auto gpu=action("Prefer GPU video decoding (experimental)",{},[this] {
        const bool enabled=findChild<QAction*>("Prefer GPU video decoding (experimental)")->isChecked();
        FFmpegReader::SetGpuEnabled(enabled); settings_.setValue("preferGpuDecode",enabled); settings_.sync();
        statusBar()->showMessage(text("Decoder preference applies to newly opened videos and uncached scans; unsupported GPU decoding falls back to CPU."));
    });
    gpu->setCheckable(true); gpu->setChecked(FFmpegReader::GpuEnabled());
    rememberReviewAction_=action("Remember last review",{},[] {}); rememberReviewAction_->setCheckable(true);
    rememberReviewAction_->setChecked(settings_.value("rememberLastReview",true).toBool());
    connect(rememberReviewAction_,&QAction::toggled,this,[this](bool enabled) {
        if(!enabled && reviewSaveCancel_) reviewSaveCancel_->store(true);
        reviewDirty_=true; settings_.setValue("rememberLastReview",enabled); settings_.sync();
        if(settings_.status()!=QSettings::NoError) statusBar()->showMessage(text("The preference could not be saved."));
    });
    auto diskCache=action("Store media cache on disk",QKeySequence(),[this] {
        auto item=findChild<QAction*>("Store media cache on disk"); const bool enabled=item->isChecked();
        if(scanning_ || sortingMetadata_ || fileOperationPending_ || clearingCache_ || closing_ || previewPool_.activeThreadCount()) {
            item->setChecked(!enabled); statusBar()->showMessage(text("Wait for scanning and previews to finish.")); return;
        }
        if(reviewSaving_) { reviewDirty_=true; if(reviewSaveCancel_) reviewSaveCancel_->store(true); if(reviewSaveWatcher_) reviewSaveWatcher_->waitForFinished(); }
        clearingCache_=true; centralWidget()->setEnabled(false); menuBar()->setEnabled(false);
        auto watcher=new QFutureWatcher<QString>(this); clearCacheWatcher_=watcher; const auto path=cachePath_;
        connect(watcher,&QFutureWatcherBase::finished,this,[this,watcher,item] {
            const auto error=watcher->result(); watcher->deleteLater(); clearCacheWatcher_=nullptr; clearingCache_=false;
            { ScanCache cache(cachePath_); item->setChecked(cache.mediaCachingEnabled()); }
            if(pendingClose_) { close(); return; }
            if(closing_) return;
            centralWidget()->setEnabled(true); menuBar()->setEnabled(true);
            statusBar()->showMessage(error.isEmpty()?text(item->isChecked()?"Media disk cache enabled.":"Media disk cache disabled and old media cache cleared; decisions and history kept."):error);
        });
        watcher->setFuture(QtConcurrent::run([path,enabled] { ScanCache cache(path); cache.setMediaCachingEnabled(enabled); return cache.error(); }));
    });
    diskCache->setCheckable(true);
    { ScanCache cache(cachePath_); diskCache->setChecked(cache.mediaCachingEnabled()); diskCache->setEnabled(cache.error().isEmpty()); }
    diskCache->setToolTip(text("Stores thumbnails and signatures on disk so later scans and previews are faster. Turning it off also clears the stored cache; settings, decisions and history are kept."));
    action("Clear scan cache",QKeySequence(),[this] {
        if(scanning_ || sortingMetadata_ || fileOperationPending_ || clearingCache_ || closing_ || previewPool_.activeThreadCount()) { statusBar()->showMessage(text("Wait for scanning and previews to finish.")); return; }
        if(reviewSaving_) { reviewDirty_=true; if(reviewSaveCancel_) reviewSaveCancel_->store(true); if(reviewSaveWatcher_) reviewSaveWatcher_->waitForFinished(); }
        clearingCache_=true; centralWidget()->setEnabled(false); menuBar()->setEnabled(false);
        auto watcher=new QFutureWatcher<QString>(this); clearCacheWatcher_=watcher; auto path=cachePath_;
        connect(watcher,&QFutureWatcher<QString>::finished,this,[this,watcher] {
            auto error=watcher->result(); watcher->deleteLater(); clearCacheWatcher_=nullptr; clearingCache_=false;
            if(pendingClose_) { close(); return; }
            if(closing_) return;
            centralWidget()->setEnabled(true); menuBar()->setEnabled(true);
            statusBar()->showMessage(error.isEmpty()?text("Scan cache cleared; decisions and history kept."):error);
        });
        watcher->setFuture(QtConcurrent::run([path] { ScanCache cache(path); cache.clearScanData(); return cache.error(); }));
    });
#ifdef Q_OS_MACOS
    // A Mac app closes its window with Cmd+W, offers full screen under View and has a Window menu.
    file->addAction(action("Close Window",QKeySequence::Close,[this] { close(); }));
    auto fullScreen=action("Enter Full Screen",QKeySequence::FullScreen,[this] { if(isFullScreen()) showNormal(); else showFullScreen(); });
    view->addSeparator(); view->addAction(fullScreen);
    connect(view,&QMenu::aboutToShow,fullScreen,[this,fullScreen] { fullScreen->setText(text(isFullScreen()?"Exit Full Screen":"Enter Full Screen")); });
    auto windowMenu=menuBar()->addMenu(text("Window")); windowMenu->setObjectName("Window");
    // Cmd+M stays with "Mark right file for cleanup", as Ctrl+M is on the other platforms.
    windowMenu->addAction(action("Minimize",{},[this] { showMinimized(); }));
    windowMenu->addAction(action("Zoom",{},[this] { if(isMaximized()) showNormal(); else showMaximized(); }));
#endif
    auto help=menuBar()->addMenu(text("Help")); help->setObjectName("Help");
    auto privacyInfo=action("Data and privacy",{},[this] {
        QMessageBox::information(this,text("Data and privacy"),text("The eye button hides previews on screen; it does not erase files. The local scan cache stores paths, file identities, hashes and compressed signatures. Older cache records may also contain thumbnails; new thumbnails are loaded when visible. Clear scan cache removes cached media data but keeps ignored pairs, review decisions and operation history. Saved review sessions contain paths, decisions and playback state. No encryption or secure erasure is provided.")+"\n\n"+
            text("Current media payloads are limited to 256 MiB and 100,000 entries. Database overhead, history and older cache generations are separate; Clear scan cache also removes older media caches and compacts the database.")+"\n\n"+
            text("Paused folder scans keep a local file list (up to 100,000 files / 32 MiB). It is removed on completion or by Clear scan cache.")+"\n\n"+
            text("Remember last review saves paths, identities, decisions and viewing state in last-review.cvsession. It contains no media or thumbnails. Clear scan cache keeps it; Tools > Settings… > Forget last review removes it and stops remembering.")+" "+
            text("While all previews are hidden, the last review is not saved.")+"\n\n"+
            text("Copy/move history keeps both paths, file identities and hashes locally for Undo. Quarantine creates a real media copy in your chosen folder; it is not encrypted. Clear scan cache keeps transfer history.")+"\n\n"+
            text("When update checks are on, Compareel asks GitHub for the number of the latest release at most once a day. Nothing about your files is sent.")+"\n\n"+
            text("Before moving files to Recycle Bin, last-cleanup.cvsession saves the review locally. File > Recover last cleanup review reopens it without repeating file operations. Clear scan cache does not remove this session; it can be removed from the local data folder.")+"\n\n"+text("Local data folder")+": "+QFileInfo(settings_.fileName()).absolutePath());
    });
    help->addAction(action("User guide",QKeySequence(),[] { QDesktopServices::openUrl(QUrl(guideUrl)); }));
    help->addAction(action("Keyboard shortcuts",QKeySequence(Qt::Key_F1),[this] {
        QStringList lines;
        for(const auto& [item,key]:actionTexts_) if(!item->shortcut().isEmpty()) {
            auto name=text(key);
#ifdef Q_OS_MACOS
            // Exit is the application menu's Quit item there, and is listed under that name.
            if(item->menuRole()==QAction::QuitRole) name=text("Quit %1").arg(QCoreApplication::applicationName());
#endif
            lines<<item->shortcut().toString(QKeySequence::NativeText)+"    "+name;
        }
        lines<<"B    "+text("Hold B on a preview to show the other file; release to return.");
        QMessageBox::information(this,text("Keyboard shortcuts"),lines.join('\n'));
    }));
    help->addAction(privacyInfo);
    auto updates=action("Check for updates…",QKeySequence(),[this] { checkForUpdates(true); });
    updates->setVisible(updateChecksAvailable()); help->addAction(updates);
    help->addAction(action("About Compareel",QKeySequence(),[this] {
        // Owner, source, the GPL's notice and where the third-party licenses are: what the licenses ask the program to show.
        const auto link=[](const QString& url,const QString& label) { return "<a href=\""+url+"\" style=\"color: "+Theme::color("@link").name()+"\">"+label.toHtmlEscaped()+"</a>"; };
        const QString home=projectUrl;
        QMessageBox::about(this,text("About Compareel"),"<h3>Compareel "+QCoreApplication::applicationVersion()+"</h3><p>"+
            text("Local video comparison and careful duplicate review.")+"</p><p>© 2026 Ensar Yanar<br>"+link(home,QString(home).remove("https://"))+"</p><p>"+
            text("Compareel is free software: you can redistribute it and modify it under the terms of the GNU General Public License, version 3. It comes with no warranty.").toHtmlEscaped()+
            "</p><p>"+link(home+"/blob/main/LICENSE",text("License"))+" · "+link(home+"/blob/main/THIRD_PARTY_NOTICES.md",text("Third-party components"))+"</p>");
    }));
    // macOS moves Quit, Preferences and About into the application menu. Qt guesses these from the English text,
    // which fails in other languages and can catch unrelated items, so the three are named and the rest stay put.
    for(auto item:findChildren<QAction*>()) item->setMenuRole(QAction::NoRole);
    for(auto [name,role]:{std::pair{"Exit",QAction::QuitRole},std::pair{"Settings…",QAction::PreferencesRole},std::pair{"About Compareel",QAction::AboutRole},std::pair{"Check for updates…",QAction::ApplicationSpecificRole}})
        if(auto item=findChild<QAction*>(name)) item->setMenuRole(role);
}
