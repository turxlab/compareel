// Self-check entry points for the --*-check command-line modes; they drive the real window.
#include <cstdlib>
#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "Language.h"
#include "Icons.h"
#include "FileOperations.h"
#include "ScanCache.h"
#include "ScanManifest.h"
#include "Csv.h"
#include "SubtitleInfo.h"
#include "SeekSlider.h"
#include "ImageReader.h"
#include "ShellIntegration.h"
#include "FFmpegReader.h"
#include "ScanConcurrency.h"
#include <QApplication>
#include <QActionGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDesktopServices>
#include <QDebug>
#include <QDialogButtonBox>
#include <QDir>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QFocusEvent>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGridLayout>
#include <QHeaderView>
#include <QLabel>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QGroupBox>
#include <QMenuBar>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QProgressDialog>
#include <QProcess>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QScrollArea>
#include <QScreen>
#include <QSqlQuery>
#include <QWindow>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QStyleOptionSlider>
#include <QSplitter>
#include <QStandardPaths>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QThread>
#include <QToolButton>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtConcurrent/QtConcurrentRun>

using Language::text;

bool MainWindow::runControlsCleanupCheck(const QString& mediaPath,const QString& screenshotPath) {
    auto wait=[](const std::function<bool()>& ready) { QElapsedTimer time; time.start(); while(!ready() && time.elapsed()<15000) { QApplication::processEvents(); QThread::msleep(10); } return ready(); };
    auto fail=[](const char* message) { qWarning()<<"Controls/cleanup check failed:"<<message; return false; };
    QDir().mkpath(QFileInfo(screenshotPath).absolutePath());
    QTemporaryDir fixture(QFileInfo(screenshotPath).absolutePath()+"/controls-cleanup-XXXXXX"); if(!fixture.isValid()) return fail("fixture");
    for(int g=0;g<4;++g) for(int slot=0;slot<3;++slot) {
        auto path=fixture.filePath(QString("%1-%2.mp4").arg(g).arg(slot)); if(!QFile::copy(mediaPath,path)) return fail("copy");
        if(g) { QFile file(path); if(!file.open(QIODevice::Append) || file.write(QByteArray(g,'x'))!=g) return fail("different group"); }
    }
    folders_={QDir::toNativeSeparators(fixture.path()).toStdWString()}; options_.minFileSize=1; options_.protectedFolders.clear(); options_.excludedFolders.clear();
    profile_->setCurrentIndex(0); sort_->setCurrentIndex(int(SortOrder::NameAsc)); startScan();
    if(!wait([&] { return !scanning_ && pendingPreviews_.empty() && left_->player->IsLoaded() && right_->player->IsLoaded(); }) || groups_.size()!=4) return fail("real scan");
    if(!linkAction_->isChecked() || findChildren<QSlider*>("playerSeek").size()!=2 || findChildren<QSlider*>().size()!=2) return fail("single link/two sliders");
    auto ready=[&] { return !left_->player->IsSeeking() && !right_->player->IsSeeking(); };
    auto closeTime=[](int64_t a,int64_t b) { return std::abs(a-b)<1500000; };
    alignmentOffset100ns_=1000000; controlSeek(right_,6000000,true);
    if(!wait(ready) || !closeTime(left_->player->GetPosition100ns(),5000000) || !closeTime(right_->player->GetPosition100ns(),6000000)) return fail("right linked seek/offset");
    controlSeek(left_,2000000,true); if(!wait(ready) || !closeTime(right_->player->GetPosition100ns(),3000000)) return fail("left linked seek");
    linkAction_->trigger(); auto previous=right_->player->GetPosition100ns(); controlSeek(left_,7000000,true);
    if(!wait(ready) || right_->player->GetPosition100ns()!=previous) return fail("independent seek");
    linkAction_->trigger(); if(!wait(ready) || !closeTime(right_->player->GetPosition100ns()-left_->player->GetPosition100ns(),alignmentOffset100ns_)) return fail("relink");
    right_->playRequested(); if(!wait([&] { return left_->player->IsPlaying() && right_->player->IsPlaying(); })) return fail("right linked play");
    right_->playRequested(); if(left_->player->IsPlaying() || right_->player->IsPlaying()) return fail("linked pause");
    right_->stepRequested(true); if(!wait([&] { return !linkedStepFromRight_.has_value() && ready(); })) return fail("linked frame step");
    if(!closeTime(right_->player->GetPosition100ns()-left_->player->GetPosition100ns(),alignmentOffset100ns_)) return fail("frame alignment");
    const auto duration=left_->player->GetDuration100ns();
    alignmentOffset100ns_=duration/2; seekBoth(duration*2,true);
    if(!wait(ready) || !closeTime(left_->player->GetPosition100ns(),duration/2) || !closeTime(right_->player->GetPosition100ns(),duration)) return fail("positive offset end clamp");
    seekBoth(duration/2-3000000,true); if(!wait(ready)) return fail("overlap end seek");
    playBoth(); if(!wait([&] { return !syncing_ && left_->player->IsPlaying() && right_->player->IsPlaying(); })) return fail("overlap play");
    if(!wait([&] { return !left_->player->IsPlaying() && !right_->player->IsPlaying(); })) return fail("overlap end must stop both");
    alignmentOffset100ns_=-duration/2; seekBoth(0,true);
    if(!wait(ready) || !closeTime(left_->player->GetPosition100ns(),duration/2) || !closeTime(right_->player->GetPosition100ns(),0)) return fail("negative offset start clamp");
    alignmentOffset100ns_=duration+10000000; auto heldPosition=left_->player->GetPosition100ns(); playBoth();
    if(syncing_ || left_->player->IsPlaying() || right_->player->IsPlaying() || left_->player->GetPosition100ns()!=heldPosition) return fail("no overlap must not start playback");
    linkAction_->trigger(); selectGroup(1); if(!linkAction_->isChecked()) return fail("new pair link default");
    for(auto glyph:{Glyph::Eye,Glyph::EyeOff,Glyph::Restart,Glyph::Link,Glyph::Unlink}) {
        auto pixels=icon(glyph).pixmap(24).toImage(); bool visible=false;
        for(int y=0;y<pixels.height();++y) for(int x=0;x<pixels.width();++x) visible|=qAlpha(pixels.pixel(x,y))!=0;
        if(!visible) return fail("SVG resource rendered");
    }
    files_[groups_[0].fileIndices[2]].protectedFile=true; filter_->setText("no-visible-results"); applyFilter();
    auto plan=cleanupPlan(); if(plan.size()!=7) return fail("all groups/protection plan");
    bool sawPlan=false;
    QTimer::singleShot(0,this,[&] {
        auto dialog=findChild<QDialog*>("bulkCleanupPlan");
        if(dialog) { sawPlan=planRows(dialog->findChild<QTreeWidget*>("cleanupPlanTree"))==7 && dialog->findChild<QPushButton*>("exportCleanupPlan"); if(!screenshotPath.isEmpty()) dialog->grab().save(screenshotPath+".plan.png"); dialog->reject(); }
    });
    auto bulkAction=findChild<QAction*>("Move other copies to Recycle Bin…"); if(!bulkAction) return fail("missing menu action");
    bulkAction->trigger(); if(!sawPlan || fileOperationPending_) return fail("real menu plan/cancel");
    auto cancelledPlan=plan; executeBulkCleanup(std::move(cancelledPlan)); bulkCancel_.store(true);
    if(!wait([&] { return !fileOperationPending_; })) return fail("cancel completion");
    for(const auto& target:plan) if(!QFileInfo::exists(QString::fromStdWString(target.file.fullPath))) return fail("cancel retained files");
    plan=cleanupPlan();
    // Mutate one keeper after planning: its group's targets must survive.
    auto invalidKeeper=plan.back().keeper.fullPath; { QFile file(QString::fromStdWString(invalidKeeper)); if(!file.open(QIODevice::Append) || file.write("changed")!=7) return fail("keeper mutation"); }
    auto readOnly=plan.front().file.fullPath; if(!QFile::setPermissions(QString::fromStdWString(readOnly),QFileDevice::ReadOwner|QFileDevice::ReadUser)) return fail("readonly fixture");
    QTimer dismiss; connect(&dismiss,&QTimer::timeout,this,[] { if(auto dialog=qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) dialog->accept(); }); dismiss.start(20);
    qInfo()<<"Cleanup check: executing planned batch";
    executeBulkCleanup(plan); if(!wait([&] { return !fileOperationPending_ && !QApplication::activeModalWidget(); })) return fail("batch completion"); dismiss.stop(); qInfo()<<"Cleanup check: batch callback completed";
    int removed=0;
    for(const auto& target:plan) {
        bool shouldKeep=target.keeper.fullPath==invalidKeeper || target.file.fullPath==readOnly;
        bool exists=QFileInfo::exists(QString::fromStdWString(target.file.fullPath));
        if(exists!=shouldKeep || files_[target.index].deleted==exists) return fail("actual removal/keeper/readonly safety");
        if(!exists) ++removed;
    }
    QFile::setPermissions(QString::fromStdWString(readOnly),QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ReadUser|QFileDevice::WriteUser);
    { ScanCache cache(cachePath_); if(cache.operationHistory().size()<7) return fail("actual attempt history"); }
    qInfo()<<"Cleanup check: history checked";
    if(removed!=4) return fail("all-group removal count");
    filter_->clear(); applyFilter(); if(!grab().save(screenshotPath)) return fail("screenshot");
    left_->setFile(nullptr,-1); right_->setFile(nullptr,-1); left_->player->Close(); right_->player->Close(); previewPool_.waitForDone();

    qInfo()<<"PASS controls/cleanup: real linked left/right seek, offset, unlink/relink, play/pause/frame step, pair reset, SVG rendering; all four groups despite filter, protected file, cancel, changed keeper, readonly target, actual trash and history";
    return true;
}

void MainWindow::runReviewResumeChild(const QString& mediaPath) {
    auto fail=[] { qCritical("FAIL review resume child"); std::_Exit(74); };
    reviewSaveTimer_.stop(); rememberReviewAction_->setChecked(true);
    const auto media=QFileInfo(settings_.fileName()).dir().filePath("media"); if(!QDir().mkpath(media)) { fail(); return; }
    for(int i=0;i<3;++i) if(!QFile::copy(mediaPath,QDir(media).filePath(QString("%1.mp4").arg(i)))) { fail(); return; }
    folders_={QDir::toNativeSeparators(media).toStdWString()}; options_.minFileSize=1; options_.extensions.clear(); options_.protectedFolders.clear(); options_.excludedFolders.clear();
    options_.crossFolderOnly=false; options_.sortOrder=SortOrder::NameAsc; sort_->setCurrentIndex(int(SortOrder::NameAsc)); profile_->setCurrentIndex(0); startScan();
    auto wait=[&](const auto& condition) { QElapsedTimer clock; clock.start(); while(!condition() && clock.elapsed()<15000) { QApplication::processEvents(QEventLoop::AllEvents,10); QThread::msleep(1); } return condition(); };
    if(!wait([&] { return !scanning_ && left_->player->IsLoaded() && right_->player->IsLoaded(); }) || groups_.size()!=1 || groups_[0].fileIndices.size()!=3) { fail(); return; }
    selectGroup(0,2); if(!wait([&] { return right_->player->IsLoaded(); })) { fail(); return; }
    linkAction_->setChecked(false); left_->player->SeekTo(left_->player->GetDuration100ns()/4); right_->player->SeekTo(right_->player->GetDuration100ns()*3/4);
    if(!wait([&] { return !left_->player->IsSeeking() && !right_->player->IsSeeking(); })) { fail(); return; }
    left_->canvas->setTransform(1.5,{11,-7}); right_->canvas->setTransform(1.5,{11,-7}); filter_->setText("mp4");
    { QSignalBlocker blocker(offset_); offset_->setValue(0.125); alignmentOffset100ns_=1250000; }
    // Reviewed, not Later: a keep or review-later decision now removes cleanup marks from both files.
    markedFiles_.insert(right_->fileIndex); files_[left_->fileIndex].protectedFile=true; setReviewDecision(ReviewDecision::Reviewed);
    if(auto wipe=findChild<QAction*>("Wipe")) wipe->trigger();
    if(!saveLastReview(true) || !wait([&] { return !reviewSaving_; }) || !QFileInfo::exists(lastReviewPath_)) { fail(); return; }
    std::_Exit(73);
}

bool MainWindow::runReviewResumeCheck(const QString& mediaPath) {
    auto fail=[](const char* message) { qCritical("FAIL review resume: %s",message); return false; };
    QTemporaryDir fixture; if(!fixture.isValid()) return fail("private root"); const auto state=fixture.filePath("state");
    QProcess child; child.start(QCoreApplication::applicationFilePath(),{"--review-resume-child","--media",QFileInfo(mediaPath).absoluteFilePath(),"--data-dir",state});
    if(!child.waitForFinished(20000) || child.exitCode()!=73) { qCritical()<<child.readAllStandardError(); return fail("actual persisted review child exit"); }
    MainWindow recovered(state); recovered.reviewSaveTimer_.stop(); recovered.show();
    auto wait=[&](const auto& condition) { QElapsedTimer clock; clock.start(); while(!condition() && clock.elapsed()<15000) { QApplication::processEvents(QEventLoop::AllEvents,10); QThread::msleep(1); } return condition(); };
    QFile saved(recovered.lastReviewPath_); if(!saved.open(QIODevice::ReadOnly)) return fail("persisted snapshot"); const auto original=saved.readAll(); saved.close();
    const auto expected=QJsonDocument::fromJson(original).object()["view"].toObject();
    if(!recovered.resumeReviewAction_->isEnabled()) return fail("resume action after crash"); recovered.resumeReviewAction_->trigger();
    if(!wait([&] { return !recovered.scanning_ && !recovered.restorePositions_ && recovered.left_->player->IsLoaded() && recovered.right_->player->IsLoaded() && !recovered.left_->player->IsSeeking() && !recovered.right_->player->IsSeeking(); })) return fail("actual menu restoration");
    if(recovered.groups_.size()!=1 || recovered.groups_[0].rightSlot!=2 || recovered.linkAction_->isChecked() || recovered.left_->canvas->zoom()!=1.5 || recovered.left_->canvas->pan()!=QPointF(11,-7) || recovered.alignmentOffset100ns_!=1250000 || recovered.filter_->text()!="mp4" || !recovered.markedFiles_.contains(recovered.right_->fileIndex) || !recovered.files_[recovered.left_->fileIndex].protectedFile) return fail("full review state");
    if(std::abs(recovered.left_->player->GetPosition100ns()-expected["leftTime"].toString().toLongLong())>2000000 || std::abs(recovered.right_->player->GetPosition100ns()-expected["rightTime"].toString().toLongLong())>2000000) return fail("independent playback positions");
    { ScanCache cache(recovered.cachePath_); if(!cache.operationHistory().empty() || recovered.reviewDecisions_.empty() || QDir(state+"/media").entryList({"*.mp4"},QDir::Files).size()!=3) return fail("no repeated file actions / durable decisions"); }
#ifdef _WIN32
    // An open handle without delete sharing makes the atomic replace fail on Windows.
    HANDLE lock=CreateFileW(QDir::toNativeSeparators(recovered.lastReviewPath_).toStdWString().c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    if(lock==INVALID_HANDLE_VALUE) return fail("locked snapshot fixture");
    recovered.reviewDirty_=true; const bool started=recovered.saveLastReview(true); const bool finished=wait([&] { return !recovered.reviewSaving_; }); CloseHandle(lock);
    if(!started || !finished || !saved.open(QIODevice::ReadOnly) || saved.readAll()!=original) return fail("failed atomic save preserves previous snapshot"); saved.close();
#endif
    recovered.rememberReviewAction_->setChecked(false); if(recovered.saveLastReview(true)) return fail("opt out must stop saves");
    auto bad=QJsonDocument::fromJson(original).object(); auto view=bad["view"].toObject(); view["linked"]="invalid"; bad["view"]=view;
    const auto path=fixture.filePath("invalid.cvsession"); QFile invalid(path); if(!invalid.open(QIODevice::WriteOnly) || invalid.write(QJsonDocument(bad).toJson())<0) return fail("invalid session fixture"); invalid.close();
    if(recovered.loadSessionFile(path) || recovered.groups_.size()!=1 || recovered.groups_[0].rightSlot!=2) return fail("invalid state must not replace active review");
    recovered.rememberReviewAction_->setChecked(true); recovered.left_->canvas->setTransform(2,{17,8}); recovered.right_->canvas->setTransform(2,{17,8}); recovered.reviewDirty_=true; recovered.close();
    if(!wait([&] { return recovered.closeReady_ && !recovered.isVisible(); }) || !saved.open(QIODevice::ReadOnly)) return fail("normal close async flush");
    const auto closed=QJsonDocument::fromJson(saved.readAll()).object()["view"].toObject(); saved.close();
    if(closed["zoom"].toDouble()!=2 || closed["panX"].toDouble()!=17 || closed["panY"].toDouble()!=8) return fail("latest close state persisted");
    MainWindow blocked(state); blocked.reviewSaveTimer_.stop(); blocked.show();
    if(!blocked.loadSessionFile(blocked.lastReviewPath_) || !wait([&] { return !blocked.scanning_ && !blocked.restorePositions_ && blocked.left_->player->IsLoaded() && blocked.right_->player->IsLoaded(); })) return fail("close failure fixture load");
#ifdef _WIN32
    lock=CreateFileW(QDir::toNativeSeparators(blocked.lastReviewPath_).toStdWString().c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    if(lock==INVALID_HANDLE_VALUE) return fail("close failure fixture lock");
    blocked.left_->canvas->setTransform(3,{29,13}); blocked.reviewDirty_=true; blocked.close();
    const bool closedAfterFailure=wait([&] { return blocked.closeReady_ && !blocked.isVisible(); }); CloseHandle(lock);
    if(!closedAfterFailure) return fail("failed close save must not loop forever");
    if(!saved.open(QIODevice::ReadOnly)) return fail("previous close snapshot"); const auto preserved=QJsonDocument::fromJson(saved.readAll()).object()["view"].toObject(); saved.close();
    if(preserved["zoom"].toDouble()!=2 || preserved["panX"].toDouble()!=17) return fail("failed close save lost previous snapshot");
#endif
    qInfo()<<"PASS review resume: actual abrupt-exit child and File resume; third candidate, zoom/pan, offset, independent positions, filter, marks/protection/decisions; zero file operations; failed atomic save keeps previous; opt-out and invalid-state refusal; normal close async flush and failed-close-save terminates without retry loop; private fixture only";
    return true;
}

bool MainWindow::runTransferCheck(const QString& mediaPath) {
    qInfo()<<"Transfer UI check started";
    auto fail=[](const char* message) { qCritical("FAIL transfer UI: %s",message); return false; };
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs,true);
    QTemporaryDir fixture; if(!fixture.isValid()) return fail("private root");
    const auto media=fixture.filePath("media"),output=fixture.filePath("output");
    if(!QDir().mkpath(media) || !QDir().mkpath(output)) return fail("private folders");
    for(int i=0;i<3;++i) if(!QFile::copy(mediaPath,QDir(media).filePath(QString("%1.mp4").arg(i)))) return fail("private media copies");
    folders_={QDir::toNativeSeparators(media).toStdWString()}; options_.minFileSize=1; options_.crossFolderOnly=false;
    options_.protectedFolders.clear(); options_.excludedFolders.clear(); options_.sortOrder=SortOrder::NameAsc;
    profile_->setCurrentIndex(0); sort_->setCurrentIndex(int(SortOrder::NameAsc)); qInfo()<<"Transfer UI starting scan"; startScan();
    auto wait=[&](const auto& condition) { QElapsedTimer clock; clock.start(); while(!condition() && clock.elapsed()<20000) { QApplication::processEvents(QEventLoop::AllEvents,10); QThread::msleep(1); } return condition(); };
    if(!wait([&] { return !scanning_ && left_->player->IsLoaded() && right_->player->IsLoaded(); }) || groups_.size()!=1 || groups_[0].fileIndices.size()!=3) return fail("actual three-file scan");
    selectGroup(0,2); const auto original=files_[right_->fileIndex];
    const auto source=QString::fromStdWString(original.fullPath),destination=QDir(output).filePath(QString::fromStdWString(original.fileName));
    QStringList warnings;
    auto drive=[&](std::function<void()> trigger,const QString& menuOperation=QString(),bool yes=true) {
        qInfo()<<"Transfer UI step"<<menuOperation<<"confirm"<<yes;
        QTimer driver; driver.setInterval(5); QElapsedTimer deadline; deadline.start();
        QPointer<QWidget> lastDialog;
        connect(&driver,&QTimer::timeout,this,[&] {
            auto modal=QApplication::activeModalWidget(); auto popup=QApplication::activePopupWidget();
            if((modal?modal:popup)!=lastDialog) { lastDialog=modal?modal:popup; if(lastDialog) qInfo()<<"Transfer UI dialog"<<lastDialog->metaObject()->className()<<lastDialog->windowTitle(); }
            if(deadline.elapsed()>20000) {
                if(auto dialog=qobject_cast<QDialog*>(modal)) dialog->reject();
                if(popup) popup->close(); driver.stop(); warnings<<"test dialog timeout"; return;
            }
            if(auto menu=qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
                auto chosen=menu->findChild<QAction*>(menuOperation,Qt::FindDirectChildrenOnly);
                if(chosen && !menu->property("testTriggered").toBool()) {
                    menu->setProperty("testTriggered",true);
                    QTimer::singleShot(0,menu,[menu,chosen] { menu->close(); chosen->trigger(); });
                }
            }
            if(auto dialog=qobject_cast<QFileDialog*>(QApplication::activeModalWidget())) {
                // selectFile(output) would step up to the parent when the dialog already has focus, as it does on macOS.
                dialog->setDirectory(output); QMetaObject::invokeMethod(dialog,"accept",Qt::QueuedConnection);
            } else if(auto question=qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                if(question->standardButtons().testFlag(QMessageBox::Yes)) question->button(yes?QMessageBox::Yes:QMessageBox::No)->click();
                else { warnings<<question->text(); question->button(QMessageBox::Ok)->click(); }
            }
        });
        driver.start(); trigger(); QApplication::processEvents(QEventLoop::AllEvents,10);
        return wait([&] { return !fileOperationPending_ && !QApplication::activeModalWidget() && !QApplication::activePopupWidget(); });
    };
    auto context=[&] {
        const auto point=right_->canvas->rect().center(); QContextMenuEvent event(QContextMenuEvent::Mouse,point,right_->canvas->mapToGlobal(point)); QApplication::sendEvent(right_->canvas,&event);
    };
    auto thumbnailContext=[&] {
        for(int g=0;g<int(groups_.size());++g) for(int slot=0;slot<int(groups_[g].fileIndices.size());++slot)
            if(groups_[g].fileIndices[slot]==right_->fileIndex) { clickThumbnail(g,slot,Qt::RightButton); return; }
    };
    files_[right_->fileIndex].protectedFile=true;
    { QMenu menu; addTransferActions(&menu,right_->fileIndex);
      if(!menu.findChild<QAction*>("copy")->isEnabled() || menu.findChild<QAction*>("move")->isEnabled() || menu.findChild<QAction*>("quarantine")->isEnabled()) return fail("protected transfer menu"); }
    if(!drive(thumbnailContext,"copy") || !warnings.isEmpty() || !QFileInfo::exists(source) || !QFileInfo::exists(destination) || !undoTransferAction_->isEnabled()) return fail("actual third thumbnail copy/protection/undo availability");
    { ScanCache cache(cachePath_); const auto record=cache.lastUndoableTransfer(); if(!record || record->sourceIdentity!=original.identity || record->operation!="copy" || record->moved) return fail("durable actual copy journal"); }
    if(!drive([&] { undoTransferAction_->trigger(); },{},false) || !QFileInfo::exists(destination)) return fail("cancel Undo");
    options_.protectedFolders={QDir::toNativeSeparators(output).toStdWString()};
    if(!drive([&] { undoTransferAction_->trigger(); }) || warnings.size()!=1 || !QFileInfo::exists(destination)) return fail("Undo respects destination folder protection");
    options_.protectedFolders.clear(); warnings.clear();
    if(!drive([&] { undoTransferAction_->trigger(); }) || !warnings.isEmpty() || QFileInfo::exists(destination) || FileOperations::ReadIdentity(original.fullPath)!=original.identity) return fail("real copy Undo preserves original");
    for(auto& file:files_) file.protectedFile=false;
    selectGroup(0,2);
    setReviewDecision(ReviewDecision::KeepBoth);
    { QMenu menu; addTransferActions(&menu,right_->fileIndex); if(menu.findChild<QAction*>("move")->isEnabled()) return fail("review protection transfer menu"); }
    startTransfer(files_[right_->fileIndex],destination,"move");
    if(fileOperationPending_ || !QFileInfo::exists(source) || QFileInfo::exists(destination)) return fail("review protection worker boundary");
    setReviewDecision(ReviewDecision::Unreviewed);
    auto rightMenu=findChild<QMenu*>("Right file"); if(!rightMenu) return fail("file menu");
    const bool moveFinished=drive([&] { rightMenu->popup(mapToGlobal(QPoint(30,30))); QApplication::processEvents(); },"move");
    if(!moveFinished || !warnings.isEmpty() || QFileInfo::exists(source) || !QFileInfo::exists(destination) || groups_.empty() || groups_[0].fileIndices.size()!=2) {
        qWarning()<<"Move details"<<moveFinished<<warnings<<QFileInfo::exists(source)<<QFileInfo::exists(destination)<<groups_.size()<<(groups_.empty()?0:groups_[0].fileIndices.size());
        ScanCache cache(cachePath_); qWarning()<<cache.operationHistory(); return fail("actual File menu move/regroup");
    }
    {
        MainWindow reopened(QFileInfo(settings_.fileName()).absolutePath()); reopened.show();
        if(!reopened.undoTransferAction_->isEnabled()) return fail("Undo survives reopening without a loaded pair");
        if(!drive([&] { reopened.undoTransferAction_->trigger(); }) || !warnings.isEmpty() || !QFileInfo::exists(source) || QFileInfo::exists(destination)) return fail("restart Undo restores moved file");
        reopened.left_->player->Close(); reopened.right_->player->Close();
    }
    for(auto& file:files_) if(file.fullPath==original.fullPath) { file.deleted=false; file.identity=FileOperations::ReadIdentity(file.fullPath); }
    refreshTransferResults(); selectGroup(0,2);
    if(!drive(context,"quarantine") || !warnings.isEmpty() || QFileInfo::exists(source)) return fail("actual quarantine menu");
    { ScanCache cache(cachePath_); const auto record=cache.lastUndoableTransfer();
      if(!record || record->operation!="quarantine" || !record->moved || !QFileInfo::exists(record->destination) || QFileInfo(record->destination).absolutePath()==output) return fail("unique quarantine folder/journal"); }
    if(!drive([&] { undoTransferAction_->trigger(); }) || !warnings.isEmpty() || !QFileInfo::exists(source) || groups_[0].fileIndices.size()!=3) return fail("quarantine Undo/regroup");
    selectGroup(0,2);
    if(!QFile::copy(source,destination)) return fail("collision fixture");
    const auto collisionIdentity=FileOperations::ReadIdentity(destination.toStdWString());
    if(!drive(context,"move") || warnings.size()!=1 || !QFileInfo::exists(source) || FileOperations::ReadIdentity(destination.toStdWString())!=collisionIdentity) return fail("collision error in actual UI/no overwrite");
    warnings.clear();
    if(settings_.value("cleanupInProgress").toBool()) return fail("normal completion marker");
    left_->setFile(nullptr,-1); right_->setFile(nullptr,-1); previewPool_.waitForDone(); left_->player->Close(); right_->player->Close();
    qInfo()<<"PASS transfer UI: private three-file scan; actual third thumbnail/canvas context and File menu; file/review/folder protection; verified copy/Undo; cancel; move/regroup; restart persistent Undo without pair; unique quarantine/Undo; occupied target refusal, journal and recovery marker; no screenshots or user files";
    return true;
}

bool MainWindow::runReviewDecisionsCheck(const QString& mediaPath) {
    auto fail=[](const char* message) { qCritical("FAIL review decisions: %s",message); return false; };
    QTemporaryDir fixture; if(!fixture.isValid()) return fail("private root");
    const auto media=fixture.filePath("media"); if(!QDir().mkpath(media)) return fail("private media root");
    for(int i=0;i<4;++i) if(!QFile::copy(mediaPath,QDir(media).filePath(QString("%1.mp4").arg(i)))) return fail("private copies");
    folders_={QDir::toNativeSeparators(media).toStdWString()}; options_.minFileSize=1; options_.crossFolderOnly=false;
    options_.protectedFolders.clear(); options_.excludedFolders.clear(); options_.sortOrder=SortOrder::NameAsc;
    profile_->setCurrentIndex(0); sort_->setCurrentIndex(int(SortOrder::NameAsc)); startScan();
    auto wait=[&](const auto& condition) { QElapsedTimer clock; clock.start(); while(!condition() && clock.elapsed()<20000) { QApplication::processEvents(QEventLoop::AllEvents,10); QThread::msleep(1); } return condition(); };
    if(!wait([&] { return !scanning_ && left_->player->IsLoaded() && right_->player->IsLoaded(); }) || groups_.size()!=1 || groups_[0].fileIndices.size()!=4) return fail("actual four-file group");
    if(thumbnailRect(0,2).isEmpty()) return fail("third thumbnail");
    bool contextSeen=false; QTimer choose; choose.setInterval(5);
    connect(&choose,&QTimer::timeout,this,[&] { if(auto menu=qobject_cast<QMenu*>(QApplication::activePopupWidget())) { contextSeen=true; reviewActions_[1]->trigger(); menu->close(); } }); choose.start();
    clickThumbnail(0,2,Qt::RightButton);
    if(!wait([&] { return contextSeen; })) return fail("actual thumbnail context menu"); choose.stop();
    if(groups_[0].rightSlot!=2 || !reviewActions_[1]->isChecked()) return fail("context menu selected pair");
    const auto pair=ReviewPairKey(files_[left_->fileIndex].identity,files_[right_->fileIndex].identity);
    const auto left=files_[left_->fileIndex].fullPath,right=files_[right_->fileIndex].fullPath;
    const auto stalePlan=cleanupPlan(); if(stalePlan.size()!=3) return fail("reviewed does not assert identity or silently change plan");
    reviewActions_[2]->trigger();
    if(!protectedByReview(files_[left_->fileIndex]) || !protectedByReview(files_[right_->fileIndex]) || cleanupPlan().size()!=2) return fail("keep both plan protection");
    deleteFile(right_); if(fileOperationPending_ || !QFileInfo::exists(QString::fromStdWString(right))) return fail("keep both single removal guard");
    executeBulkCleanup(stalePlan);
    if(!wait([&] { return !fileOperationPending_; }) || !QFileInfo::exists(QString::fromStdWString(left)) || !QFileInfo::exists(QString::fromStdWString(right)) || QDir(media).entryList({"*.mp4"},QDir::Files).size()!=2) return fail("stale bulk plan honors keep decision");
    reviewActions_[3]->trigger(); if(!cleanupPlan().empty() || !groupLabel_->text().contains(text("Review later"))) return fail("review later protection/visible status");
    const auto session=fixture.filePath("review.cvsession"); if(!saveSessionFile(session)) return fail("decision session save");
    { ScanCache cache(cachePath_); cache.clearScanData(); if(!cache.error().isEmpty() || cache.reviewDecisions().at(pair)!=ReviewDecision::Later) return fail("cache clear must preserve decisions"); }
    MainWindow imported(fixture.filePath("import-state")); imported.show();
    if(!imported.loadSessionFile(session) || !wait([&] { return !imported.scanning_ && !imported.restorePositions_ && imported.left_->player->IsLoaded() && imported.right_->player->IsLoaded(); }) || !imported.cleanupPlan().empty() || imported.reviewDecisions_[pair]!=ReviewDecision::Later) return fail("fresh-state session import protection");
    imported.reviewActions_[1]->trigger();
    if(imported.cleanupPlan().size()!=1 || !imported.loadSessionFile(session) || !wait([&] { return !imported.scanning_; }) || imported.reviewDecisions_[pair]!=ReviewDecision::Reviewed) return fail("newer local decision survives older session");
    QFile saved(session); if(!saved.open(QIODevice::ReadOnly)) return fail("read session fixture"); const auto object=QJsonDocument::fromJson(saved.readAll()).object(); saved.close();
    for(auto bad:QList<QJsonValue>{QJsonObject{{QString::fromStdString(pair),4}},QJsonObject{{"unknown/pair",1}},QJsonArray{1}}) {
        auto edited=object; edited["reviewDecisions"]=bad; const auto invalid=fixture.filePath("invalid.cvsession");
        QFile output(invalid); if(!output.open(QIODevice::WriteOnly)) return fail("invalid fixture write"); output.write(QJsonDocument(edited).toJson()); output.close();
        const auto generation=imported.generation_;
        if(imported.loadSessionFile(invalid) || imported.generation_!=generation || imported.reviewDecisions_[pair]!=ReviewDecision::Reviewed) return fail("invalid review state must not mutate active session");
    }
    {
        auto legacy=object; legacy.remove("reviewDecisions"); const auto path=fixture.filePath("legacy.cvsession");
        QFile output(path); if(!output.open(QIODevice::WriteOnly)) return fail("legacy fixture write"); output.write(QJsonDocument(legacy).toJson()); output.close();
        MainWindow legacyState(fixture.filePath("legacy-state"));
        if(!legacyState.loadSessionFile(path) || !wait([&] { return !legacyState.scanning_; }) || legacyState.groups_.size()!=1 || !legacyState.reviewDecisions_.empty()) return fail("old sessions without review decisions");
    }
    { MainWindow reopened(fixture.filePath("import-state")); if(reopened.reviewDecisions_[pair]!=ReviewDecision::Reviewed) return fail("decision survives application reopening"); }
    reviewActions_[0]->trigger(); if(cleanupPlan().size()!=1 || reviewDecisions_.contains(pair)) return fail("clear selected decision");
    const auto missing=std::find_if(stalePlan.begin(),stalePlan.end(),[&](const auto& target) { return target.file.fullPath!=right && !QFileInfo::exists(QString::fromStdWString(target.file.fullPath)); });
    if(missing==stalePlan.end()) return fail("missing old partner fixture");
    std::set<std::string> ignored; QStringList history;
    { ScanCache cache(cachePath_); cache.saveReviewDecisions({{ReviewPairKey(files_[right_->fileIndex].identity,missing->file.identity),ReviewDecision::Later}});
      cache.ignorePair(files_[left_->fileIndex],files_[right_->fileIndex]); ignored=cache.ignoredPairs(); history=cache.operationHistory(); }
    refreshReviewDecisions(); if(!cleanupPlan().empty()) return fail("old missing partner still protects survivor");
    files_[left_->fileIndex].protectedFile=files_[left_->fileIndex].protectedByFolder=true; files_[right_->fileIndex].protectedFile=true;
    auto reset=findChild<QAction*>("Reset all review decisions…"); if(!reset) return fail("reset command");
    for(auto answer:{QMessageBox::No,QMessageBox::Yes}) {
        bool prompted=false; QTimer::singleShot(0,this,[&] { if(auto dialog=qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) { prompted=true; dialog->button(answer)->click(); } });
        reset->trigger();
        if(!prompted || reviewDecisions_.empty()!=(answer==QMessageBox::Yes)) return fail("reset confirmation/cancel");
    }
    { ScanCache cache(cachePath_); if(!cache.reviewDecisions().empty() || cache.ignoredPairs()!=ignored || cache.operationHistory()!=history) return fail("reset must preserve ignored pairs/history"); }
    if(!files_[left_->fileIndex].protectedByFolder || !files_[left_->fileIndex].protectedFile || !files_[right_->fileIndex].protectedFile || !cleanupPlan().empty()) return fail("reset must preserve file/folder locks");
    files_[right_->fileIndex].protectedFile=false; if(cleanupPlan().size()!=1) return fail("orphan protection cleared");
    reviewActions_[3]->trigger(); auto changed=files_[right_->fileIndex];
    for(auto window:{this,&imported}) {
        window->left_->setFile(nullptr,-1); window->right_->setFile(nullptr,-1); window->previewPool_.waitForDone(); window->left_->player->Close(); window->right_->player->Close();
    }
    { QFile output(QString::fromStdWString(changed.fullPath)); if(!output.open(QIODevice::Append) || output.write("changed")!=7) return fail("changed-identity fixture"); }
    changed.identity=FileOperations::ReadIdentity(changed.fullPath);
    if(!changed.identity.valid || protectedByReview(changed)) return fail("old decision applied to changed file identity");
    qInfo()<<"PASS review decisions: real third-thumbnail context menu, reviewed/keep/later/clear, single and stale bulk removal guards, visible status, local restart, cache clear, session import/existing-local precedence, invalid session rejection, changed identity invalidation; orphan reset confirmation preserves locks/ignored/history; private copies only, no screenshots";
    return true;
}

void MainWindow::runCleanupCrashChild(const QString& mediaPath) {
    auto fail=[] { qCritical("FAIL cleanup child"); std::_Exit(74); };
    const auto root=QFileInfo(settings_.fileName()).dir().filePath("copies");
    if(!QDir().mkpath(root)) { fail(); return; }
    for(int i=0;i<32;++i) if(!QFile::copy(mediaPath,QDir(root).filePath(QString("copy-%1.mp4").arg(i,2,10,QChar('0'))))) { fail(); return; }
    folders_={QDir::toNativeSeparators(root).toStdWString()}; options_.minFileSize=1; options_.crossFolderOnly=false;
    options_.protectedFolders.clear(); options_.excludedFolders.clear(); options_.sortOrder=SortOrder::NameAsc;
    profile_->setCurrentIndex(0); sort_->setCurrentIndex(int(SortOrder::NameAsc)); startScan();
    auto wait=[&](const auto& condition) { QElapsedTimer clock; clock.start(); while(!condition() && clock.elapsed()<20000) { QApplication::processEvents(QEventLoop::AllEvents,10); QThread::msleep(1); } return condition(); };
    if(!wait([&] { return !scanning_; }) || files_.size()!=32 || groups_.size()!=1) { fail(); return; }
    // Moving to the macOS Trash is a fast rename, so a full plan can finish before the first progress update;
    // half the plan leaves files behind whenever the exit lands.
    auto plan=cleanupPlan(); plan.resize(plan.size()/2);
    executeBulkCleanup(std::move(plan));
    auto progress=findChild<QProgressDialog*>("bulkCleanupProgress");
    auto bar=progress?progress->findChild<QProgressBar*>():nullptr;
    if(!bar || !fileOperationPending_) { fail(); return; }
    connect(bar,&QProgressBar::valueChanged,this,[](int value) {
        // Exit from the real batch progress callback, after a committed operation.
        if(value>=1) std::_Exit(73);
    });
    wait([&] { return !fileOperationPending_; }); fail();
}

bool MainWindow::runCleanupRecoveryCheck(const QString& mediaPath) {
    auto fail=[](const char* message) { qCritical("FAIL cleanup recovery: %s",message); return false; };
    QTemporaryDir fixture; if(!fixture.isValid()) return fail("private root");
    const auto state=fixture.filePath("state"); QProcess child;
    child.start(QCoreApplication::applicationFilePath(),{"--cleanup-crash-child","--media",QFileInfo(mediaPath).absoluteFilePath(),"--data-dir",state});
    if(!child.waitForFinished(30000) || child.exitCode()!=73) { qCritical()<<child.readAllStandardError(); return fail("abrupt actual UI batch child"); }
    const auto copies=state+"/copies";
    const auto remaining=QDir(copies).entryList({"copy-*.mp4"},QDir::Files);
    if(remaining.size()<2 || remaining.size()>=32 || !remaining.contains("copy-00.mp4")) return fail("partial batch/keeper filesystem state");
    const auto original=FileOperations::ReadIdentity(QFileInfo(mediaPath).absoluteFilePath().toStdWString());
    MainWindow recovered(state); recovered.show(); QApplication::processEvents();
    if(!recovered.settings_.value("cleanupInProgress").toBool() || !recovered.recoverCleanupAction_->isEnabled() || !QFileInfo::exists(recovered.cleanupReviewPath_)) return fail("recovery marker/snapshot/menu");
    QStringList history; { ScanCache cache(recovered.cachePath_); history=cache.operationHistory(); if(history.empty() || !cache.error().isEmpty()) return fail("reopened operation journal"); }
    recovered.recoverCleanupAction_->trigger();
    auto wait=[&](const auto& condition) { QElapsedTimer clock; clock.start(); while(!condition() && clock.elapsed()<20000) { QApplication::processEvents(QEventLoop::AllEvents,10); QThread::msleep(1); } return condition(); };
    if(!wait([&] { return !recovered.scanning_ && !recovered.restorePositions_ && recovered.left_->player->IsLoaded() && recovered.right_->player->IsLoaded(); }) || recovered.groups_.size()!=1) return fail("actual menu review restoration");
    const auto live=std::count_if(recovered.files_.begin(),recovered.files_.end(),[](const auto& file) { return !file.deleted; });
    if(live!=remaining.size() || recovered.settings_.value("cleanupInProgress").toBool() || QDir(copies).entryList({"copy-*.mp4"},QDir::Files)!=remaining) return fail("restore must acknowledge interruption and exclude missing files without repeating operations");
    { ScanCache cache(recovered.cachePath_); if(cache.operationHistory()!=history) return fail("restore changed operation history"); }
    const auto plan=recovered.cleanupPlan(); if(plan.size()!=size_t(remaining.size()-1)) return fail("remaining cleanup plan");
    const auto backup=recovered.cleanupReviewPath_+".saved";
    if(!QFile::rename(recovered.cleanupReviewPath_,backup) || !QDir().mkdir(recovered.cleanupReviewPath_)) return fail("blocked recovery destination fixture");
    bool warned=false;
    QTimer::singleShot(0,&recovered,[&] { if(auto warning=qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) { warned=true; warning->accept(); } });
    recovered.executeBulkCleanup(plan);
    if(!warned || recovered.fileOperationPending_ || QDir(copies).entryList({"copy-*.mp4"},QDir::Files)!=remaining) return fail("snapshot failure must block moves");
    { ScanCache cache(recovered.cachePath_); if(cache.operationHistory()!=history) return fail("snapshot failure must not start journal operations"); }
    if(!QDir().rmdir(recovered.cleanupReviewPath_) || !QFile::rename(backup,recovered.cleanupReviewPath_)) return fail("restore recovery destination fixture");
    recovered.executeBulkCleanup(plan);
    if(!wait([&] { return !recovered.fileOperationPending_; }) || recovered.settings_.value("cleanupInProgress").toBool() || QDir(copies).entryList({"copy-*.mp4"},QDir::Files)!=QStringList{"copy-00.mp4"}) return fail("explicit remaining batch completion/keeper");
    const auto keeper=QDir(copies).filePath("copy-00.mp4"),single=QDir(copies).filePath("single.mp4");
    if(!QFile::copy(keeper,single)) return fail("single-operation private copy");
    recovered.openFiles({keeper,single});
    if(!wait([&] { return recovered.left_->player->IsLoaded() && recovered.right_->player->IsLoaded(); })) return fail("single-operation actual players");
    recovered.deleteFile(recovered.right_);
    if(!wait([&] { return !recovered.fileOperationPending_; }) || QFileInfo::exists(single) || !QFileInfo::exists(keeper) || recovered.settings_.value("cleanupInProgress").toBool()) return fail("single-operation recovery completion");
    if(!original.valid || FileOperations::ReadIdentity(QFileInfo(mediaPath).absoluteFilePath().toStdWString())!=original) return fail("source changed");
    qInfo()<<"PASS cleanup recovery: actual batch process killed after progress;"<<32-remaining.size()<<"moves before crash; saved review/menu/journal restored; no automatic retry; snapshot failure blocks moves; explicit remaining batch and single Trash keep first; marker cleared; private fixture only, no screenshots";
    return true;
}

bool MainWindow::runFeedbackCheck(const QString& mediaPath,const QString& screenshotPath) {
    if(auto desktop=screen()) qInfo()<<"DPI check: available"<<desktop->availableGeometry()<<"scale"<<desktop->devicePixelRatio()<<"frame"<<frameGeometry()<<"minimum hint"<<minimumSizeHint();
    if(screen() && !screen()->availableGeometry().contains(frameGeometry())) return false;
    auto wait=[](const std::function<bool()>& ready) {
        QElapsedTimer elapsed; elapsed.start();
        while(!ready() && elapsed.elapsed()<15000) {
            QApplication::processEvents(); QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete); QThread::msleep(10);
        }
        return ready();
    };
    auto fail=[](const char* what) { qWarning()<<"Feedback check failed:"<<what; return false; };
    // With no pair the comparison controls give way to the empty state; the offset must stay disabled.
    if(offset_->isEnabled() || !emptyState_->isVisible()) return fail("empty state shown and pair offset disabled");
    QApplication::processEvents();
    // The welcome card focuses the next expected step: choosing sources, or scanning once there are some.
    if(auto focus=QApplication::focusWidget(); !focus || focus->objectName()!=(folders_.empty()?"emptySourcesButton":"emptyScanButton")) return fail("welcome card initial focus");
    {
        // A completed scan remembered from an earlier run is not "no matches" at startup.
        const auto savedFolders=folders_; const auto savedOutcome=lastScanOutcome_; const bool savedResume=resumeReviewAction_->isEnabled(),savedCompleted=scanCompletedThisSession_;
        auto title=emptyState_->findChild<QLabel*>("emptyTitle");
        folders_={L"C:\\Media"}; lastScanOutcome_="completed"; scanCompletedThisSession_=false; updateEmptyState();
        if(!title || title->text()!=text("Find duplicate videos and images")) return fail("remembered scan shown as no matches at startup");
        if(const auto shots=qEnvironmentVariable("COMPAREEL_CHECK_SHOTS"); !shots.isEmpty()) {
            folders_=savedFolders; updateEmptyState(); QApplication::processEvents(); grab().save(shots+"/empty-state.png");
            // The "no matches" variant, with a resumable review, is the busiest version of the card.
            folders_={L"C:\\Media"}; scanCompletedThisSession_=true; resumeReviewAction_->setEnabled(true); updateEmptyState();
            for(int i=0;i<10;++i) { QApplication::processEvents(); QThread::msleep(20); }
            grab().save(shots+"/empty-state-no-matches.png");
        }
        folders_=savedFolders; lastScanOutcome_=savedOutcome; scanCompletedThisSession_=savedCompleted; resumeReviewAction_->setEnabled(savedResume); updateEmptyState(); QApplication::processEvents();
    }
    auto colored=[](const QImage& image) {
        for(int y=0;y<image.height();y+=4) for(int x=0;x<image.width();x+=4) {
            auto c=image.pixelColor(x,y);
            if(std::max({c.red(),c.green(),c.blue()})>220 && std::max({c.red(),c.green(),c.blue()})-std::min({c.red(),c.green(),c.blue()})>160) return true;
        }
        return false;
    };
    QDir().mkpath(QFileInfo(screenshotPath).absolutePath());
    QTemporaryDir fixture(QFileInfo(screenshotPath).absolutePath()+"/feedback-files-XXXXXX");
    if(!fixture.isValid()) return fail("fixture directory");
    QStringList names{"A0_long_filename_for_middle_elision_reference_original.mp4","A1_long_filename_for_middle_elision_candidate_copy.mp4",
        "A2_long_filename_for_middle_elision_candidate_third.mp4","B0_second_group.mp4","B1_second_group.mp4"};
    for(const auto& name:names) {
        auto path=fixture.filePath(name); if(!QFile::copy(mediaPath,path)) return fail("fixture copy");
        if(name.startsWith('B')) { QFile file(path); if(!file.open(QIODevice::Append) || file.write("x")!=1) return fail("fixture mutation"); }
    }
    { QSignalBlocker a(hideVideosAction_),b(hideThumbnailsAction_); hideVideosAction_->setChecked(false); hideThumbnailsAction_->setChecked(false); }
    applyPrivacy(); profile_->setCurrentIndex(0); sort_->setCurrentIndex(int(SortOrder::NameAsc));
    folders_={QDir::toNativeSeparators(fixture.path()).toStdWString()}; options_.minFileSize=1; options_.protectedFolders.clear(); options_.excludedFolders.clear();
    startScan();
    if(!wait([&] { return !scanning_ && left_->player->IsLoaded() && right_->player->IsLoaded(); }) || groups_.size()!=2) return fail("scan/load");
    if(!similarity_->toolTip().contains("100%") || !similarity_->toolTip().contains(text("Exact copy · 100% identical file contents"))) { qCritical("verdict: %s",qPrintable(similarity_->toolTip())); return fail("exact similarity"); }
    const auto caption=thumbnailView(0,0).caption;
    if(groups_[0].fileIndices.size()!=3 || !caption.startsWith("A0") || !caption.endsWith(".mp4") || !caption.contains(QChar(0x2026))) return fail("middle elision/extension");
    if(!results_->findChildren<QCheckBox*>().empty()) return fail("unexpected thumbnail checkbox");
    auto key=[&](int code) {
        auto target=QApplication::focusWidget();
        QKeyEvent event(QEvent::KeyPress,code,Qt::NoModifier); QApplication::sendEvent(target?target:results_,&event);
        QApplication::processEvents(); QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
    };
    results_->setFocus(Qt::OtherFocusReason);
    key(Qt::Key_Right); if(groups_[0].rightSlot!=2 || right_->fileIndex!=groups_[0].fileIndices[2]) return fail("right arrow");
    key(Qt::Key_Down); if(currentGroup_!=1) return fail("down arrow/focus");
    key(Qt::Key_Up); if(currentGroup_!=0 || groups_[0].rightSlot!=2) return fail("up arrow/remembered candidate");
    key(Qt::Key_Left); if(groups_[0].rightSlot!=1) return fail("left arrow");
    key(Qt::Key_End); if(groups_[0].rightSlot!=2) return fail("end key");
    key(Qt::Key_Home); if(groups_[0].rightSlot!=1) return fail("home key");
    filter_->setText("B0_second"); key(Qt::Key_Down); if(currentGroup_!=1) return fail("filtered groups"); filter_->clear();
    qInfo()<<"PASS feedback: filename middle elision/extension, no thumbnail checkboxes, actual arrow/Home/End events and focus";
    hideVideosAction_->trigger();
    if(colored(left_->canvas->grab().toImage()) || colored(right_->canvas->grab().toImage()) || hideThumbnailsAction_->isChecked()) {
        qWarning()<<"Privacy state"<<hideVideosAction_->isChecked()<<hideThumbnailsAction_->isChecked();
        left_->canvas->grab().save(screenshotPath+".privacy-failed.png"); return fail("video privacy");
    }
    hideThumbnailsAction_->trigger();
    selectGroup(0,2);
    if(!wait([&] { return pendingPreviews_.empty(); })) return fail("privacy pending previews");
    for(int g=0;g<int(groups_.size());++g) for(int slot=0;slot<int(groups_[g].fileIndices.size());++slot) {
        const auto view=thumbnailView(g,slot); if(!view.previewHidden || view.hasImage) return fail("late preview bypassed privacy");
    }
    if(!privacyAction_->isChecked() || colored(left_->canvas->grab().toImage()) || colored(right_->canvas->grab().toImage())) return fail("both privacy");
    { MainWindow restored(QFileInfo(settings_.fileName()).absolutePath());
      if(!restored.hideVideosAction_->isChecked() || !restored.hideThumbnailsAction_->isChecked()) return fail("privacy persistence"); }
    if(!screenshotPath.isEmpty() && !grab().save(screenshotPath+".privacy.png")) return fail("privacy screenshot");
    privacyAction_->trigger();
    if(hideVideosAction_->isChecked() || hideThumbnailsAction_->isChecked()) return fail("global privacy restore");
    if(!wait([&] { return left_->player->IsLoaded() && right_->player->IsLoaded(); }) || !colored(left_->canvas->grab().toImage())) return fail("restore previews");
    qInfo()<<"PASS feedback: independent/common privacy, late preview delivery, persistent settings and restore";
    auto geometryValid=[&] {
        for(auto panel:{left_,right_}) {
            auto seek=panel->findChild<QSlider*>("playerSeek"); auto name=panel->findChild<QLineEdit*>("filename"); auto folder=panel->findChild<QLineEdit*>("folderPath");
            QRect canvas(panel->canvas->mapTo(panel,QPoint(0,0)),panel->canvas->size());
            QRect transport(seek->mapTo(panel,QPoint(0,0)),seek->size());
            QRect info(folder->mapTo(panel,QPoint(0,0)),folder->size());
            QRect heading(name->mapTo(panel,QPoint(0,0)),name->size());
            if(canvas.height()<48 || canvas.intersects(transport) || canvas.intersects(info) || canvas.bottom()>=transport.top() || heading.bottom()>=canvas.top() || transport.bottom()>=info.top() || !panel->rect().contains(info)) { qWarning()<<"geometry: panel"<<panel->rect()<<"heading"<<heading<<"canvas"<<canvas<<"transport"<<transport<<"info"<<info; return false; }
        }
        auto link=findChild<QToolButton*>("linkControls");
        if(link->toolButtonStyle()!=Qt::ToolButtonIconOnly || link->width()!=28 || link->parentWidget()->width()!=32) { qWarning()<<"geometry: link"<<link->size()<<link->parentWidget()->size(); return false; }
        auto linkCenter=link->mapToGlobal(link->rect().center());
        for(auto panel:{left_,right_}) {
            auto seek=panel->findChild<QSlider*>("playerSeek");
            if(std::abs(seek->mapToGlobal(seek->rect().center()).y()-linkCenter.y())>2) { qWarning()<<"geometry: link/seek centre"<<seek->mapToGlobal(seek->rect().center())<<linkCenter; return false; }
        }
        if(groupLabel_->parentWidget()->objectName()!="resultArea") return false;
        return true;
    };
    auto desktopMaximum=screen()?screen()->availableGeometry().size()-(frameGeometry().size()-this->size())-QSize(2,2):QSize(1360,920);
    for(auto requested:{QSize(1040,720),QSize(1040,800),QSize(1360,920)}) {
        const auto size=requested.boundedTo(desktopMaximum);
        resize(size); QApplication::processEvents();
        vertical_->setSizes({230,600}); QApplication::processEvents();
        if(this->size()!=size || !geometryValid()) { qWarning()<<"compact size"<<this->size()<<"requested"<<size<<"minimum"<<minimumSizeHint(); return fail("compact layout overlap/size"); }
        if(requested.height()==720 && !screenshotPath.isEmpty() && !grab().save(screenshotPath+".720.png")) return fail("compact screenshot");
    }
    resize(QSize(1360,920).boundedTo(desktopMaximum)); vertical_->setSizes({600,240}); QApplication::processEvents();
    if(!geometryValid() || !grab().save(screenshotPath)) return fail("normal layout");
    qInfo()<<"PASS feedback: screen-bounded 1040x720/800 and 1360x920, compressed splitter, canvas stays above transport/metadata";
    auto alignment=findChild<QWidget*>("alignmentControls"); bool resetWorked=false;
    if(!wait([&] { return offset_->isEnabled(); }) || !offset_->isVisible() || findChild<QMenu*>("alignmentPopup")) return fail("inline offset visible/enabled without popup");
    offset_->setValue(0.5);
    auto arrowClick=[&](QPoint point) {
        QMouseEvent press(QEvent::MouseButtonPress,QPointF(point),QPointF(offset_->mapToGlobal(point)),Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease,QPointF(point),QPointF(offset_->mapToGlobal(point)),Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
        QApplication::sendEvent(offset_,&press); QApplication::sendEvent(offset_,&release);
    };
    arrowClick(QPoint(offset_->width()-10,6)); if(std::abs(offset_->value()-0.6)>0.00001) return fail("actual up arrow");
    arrowClick(QPoint(offset_->width()-10,21)); if(std::abs(offset_->value()-0.5)>0.00001) return fail("actual down arrow");
    findChild<QToolButton*>("resetAlignment")->click(); resetWorked=offset_->value()==0 && alignmentOffset100ns_==0;
    offset_->setValue(0.25);
    QApplication::processEvents();
    if(!rect().contains(QRect(alignment->mapTo(this,QPoint(0,0)),alignment->size())) || alignment->width()>offset_->width()+findChild<QLabel*>("offsetLabel")->sizeHint().width()+55 || alignment->height()>28) return fail("compact inline offset geometry");
    alignment->grab().save(screenshotPath+".alignment.png"); grab().save(screenshotPath+".alignment-position.png");
    if(!resetWorked || alignmentOffset100ns_!=2500000) return fail("inline offset/reset");
    selectGroup(1); if(offset_->value()!=0) return fail("new pair offset reset"); selectGroup(0,2);
    if(!wait([&] { return left_->player->IsLoaded() && right_->player->IsLoaded(); })) return fail("compact pair reload");
    Language::setCurrent("tr"); translate(); populateResults(); QApplication::processEvents();
    if(!geometryValid() || findChild<QLabel*>("offsetLabel")->text()!="Ofset" || !groupLabel_->accessibleName().startsWith("2 gruptan 1")) {
        qWarning()<<"Turkish details"<<geometryValid()<<findChild<QLabel*>("offsetLabel")->text()<<groupLabel_->accessibleName();
        auto link=findChild<QToolButton*>("linkControls");
        for(auto panel:{left_,right_}) { auto seek=panel->findChild<QSlider*>("playerSeek"); qWarning()<<"transport/link"<<seek->mapToGlobal(seek->rect().center())<<link->mapToGlobal(link->rect().center()); }
        grab().save(screenshotPath+".tr-failed.png"); return fail("Turkish compact layout");
    }
    alignment->grab().save(screenshotPath+".alignment-tr.png");
    linkAction_->trigger(); if(linkAction_->isChecked() || !linkAction_->toolTip().contains(nativeShortcuts("Ctrl+L"))) return fail("unlink status");
    if(!grab().save(screenshotPath+".tr-unlinked.png")) return fail("Turkish/unlinked screenshot");
    linkAction_->trigger(); Language::setCurrent("en"); translate(); populateResults(); QApplication::processEvents();
    qInfo()<<"PASS compact: icon-only 32px link in 36px gap, aligned with both transports at all sizes; result navigation; inline offset with actual mouse up/down, reset and new-pair reset; Turkish layout and unlink state";

    openFiles({fixture.filePath(names[0]),fixture.filePath(names[3])});
    if(!wait([&] { return files_.size()==2 && files_[0].samples.size()==5 && files_[1].samples.size()==5 && pendingPreviews_.empty(); })) return fail("manual five samples");
    if(!similarity_->toolTip().contains("100.0%") || similarity_->toolTip().contains(text("Exact copy · 100% identical file contents"))) return fail("manual visual score");
    if(!grab().save(screenshotPath+".visual.png")) return fail("visual score screenshot");
    qInfo()<<"PASS feedback: manual pair sampled asynchronously; same pictures/different file bytes show visual 100.0%, not SHA identity";
    left_->setFile(nullptr,-1); right_->setFile(nullptr,-1); left_->player->Close(); right_->player->Close(); previewPool_.waitForDone();

    return true;
}
