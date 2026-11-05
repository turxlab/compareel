// Self-check entry points for the --*-check command-line modes; they drive the real window.
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

bool MainWindow::runResultsCheck(const QString& mediaPath,const QString& screenshotPath) {
    auto wait=[](const std::function<bool()>& ready) {
        QElapsedTimer time; time.start();
        while(!ready() && time.elapsed()<15000) {
            QApplication::processEvents(); QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete); QThread::msleep(10);
        }
        return ready();
    };
    QDir().mkpath(QFileInfo(screenshotPath).absolutePath());
    QTemporaryDir fixture(QFileInfo(screenshotPath).absolutePath()+"/results-files-XXXXXX");
    if(!fixture.isValid()) return false;
    for(int group=0;group<8;++group) for(int copy=0;copy<2;++copy) {
        auto path=fixture.filePath(QString("Group-%1-%2.mp4").arg(group).arg(copy));
        if(!QFile::copy(mediaPath,path)) return false;
        QFile file(path); if(!file.open(QIODevice::Append) || file.write(QByteArray(1,char(group)))!=1) return false;
    }
    folders_={QDir::toNativeSeparators(fixture.path()).toStdWString()};
    options_.minFileSize=1; options_.protectedFolders.clear(); options_.excludedFolders.clear();
    profile_->setCurrentIndex(0); sort_->setCurrentIndex(int(SortOrder::NameAsc));
    startScan();
    if(!wait([&] { return !scanning_ && pendingPreviews_.empty(); }) || groups_.size()!=8) return false;
    auto unseen=groups_.back().fileIndices;
    if(files_[unseen[0]].metadataLoaded || files_[unseen[1]].metadataLoaded) return false;
    bool heartbeat=false; QTimer::singleShot(0,this,[&] { heartbeat=true; });
    QElapsedTimer latency; latency.start();
    sort_->setCurrentIndex(int(SortOrder::DurationDesc));
    auto returnedMs=latency.elapsed();
    if(!sortingMetadata_ || returnedMs>100 || !wait([&] { return heartbeat; })) return false;
    if(!wait([&] { return !sortingMetadata_ && pendingPreviews_.empty(); })) return false;
    for(const auto& group:groups_) for(int index:group.fileIndices) if(!files_[index].metadataLoaded) return false;
    int last=int(groups_.size())-1; unseen=groups_[last].fileIndices;
    if(files_[unseen[0]].frameLoaded || files_[unseen[1]].frameLoaded) return false;
    int active=currentGroup_;
    results_->scrollToItem(results_->topLevelItem(last),QAbstractItemView::PositionAtBottom);
    if(!wait([&] { return files_[unseen[0]].frameLoaded && files_[unseen[1]].frameLoaded; }) || currentGroup_!=active) return false;
    if(groups_[last].fileIndices.size()!=2 || !thumbnailView(last,0).hasImage || !thumbnailView(last,1).hasImage) return false;
    QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
    if(!grab().save(screenshotPath)) return false;
    qInfo()<<"PASS results: cold exact metadata sort returned in"<<returnedMs<<"ms, UI heartbeat, eighth group thumbnails loaded by scrolling without selection";
    return true;
}

bool MainWindow::runBrandingCheck(const QString& screenshotPath) {
    auto fail=[](const char* message) { qCritical("FAIL branding: %s",message); return false; };
    if(QCoreApplication::applicationName()!="Compareel" || !windowTitle().startsWith("Compareel ")) return fail("application/window name");
    for(int size:{16,24,32,48,64,128,256}) {
        const auto image=windowIcon().pixmap(size,size).toImage();
        if(image.isNull() || !image.hasAlphaChannel() || image.pixelColor(0,0).alpha()!=0) return fail("embedded transparent icon sizes");
    }
    const auto originalLanguage=Language::current();
    for(bool turkish:{false,true}) {
        Language::setCurrent(turkish?"tr":"en"); translate();
        bool shown=false,looked=false;
        QTimer::singleShot(100,this,[&] {
            // macOS shows About as a modeless window, so it is not always the active modal widget.
            auto box=qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            for(auto widget:QApplication::topLevelWidgets()) if(!box && widget->isVisible()) box=qobject_cast<QMessageBox*>(widget);
            looked=true;
            if(box) {
                // macOS alerts have no title, and Qt ignores the one set on a message box there.
#ifdef Q_OS_MACOS
                const bool titled=box->windowTitle().isEmpty();
#else
                const bool titled=box->windowTitle()==text("About Compareel");
#endif
                shown=titled && box->text().contains("Compareel "+QCoreApplication::applicationVersion()) &&
                    box->text().contains(QString::fromUtf8("\u00a9 2026 Ensar Yanar")) && box->text().contains("href=\""+QString(projectUrl)+"\"") && !box->iconPixmap().isNull();
                bool link=false;
                for(auto label:box->findChildren<QLabel*>()) if(label->text().contains(QString(projectUrl)+"/blob/main/LICENSE") && label->openExternalLinks()) link=true;
                shown=shown && link;
                if(!screenshotPath.isEmpty()) shown=box->grab().save(screenshotPath+(turkish?".about-tr.png":".about-en.png")) && shown;
                box->accept();
            }
        });
        auto about=findChild<QAction*>("About Compareel"); if(!about) return fail("About action");
        about->trigger();
        QElapsedTimer waited; waited.start();
        while(!looked && waited.elapsed()<3000) QApplication::processEvents(QEventLoop::AllEvents,20);
        if(!shown) return fail("real About dialog, owner, website or icon");
    }
    Language::setCurrent(originalLanguage); translate();
    QTemporaryDir fixture; if(!fixture.isValid()) return fail("temporary session directory");
    const auto path=fixture.filePath("branding.cvsession");
    manual_=false; files_.clear(); folders_.clear(); scanCriteria_=activeCriteria();
    if(!saveSessionFile(path)) return fail("new session save");
    QFile saved(path); if(!saved.open(QIODevice::ReadOnly)) return fail("read session");
    auto document=QJsonDocument::fromJson(saved.readAll()).object(); saved.close();
    if(document["format"].toString()!="Compareel.session") return fail("new session identity");
    struct SessionCase { const char* format; int version; bool expected; };
    for(const auto& [format,version,expected]:{SessionCase{"Compareel.session",1,true},SessionCase{"Compareel.session",3,true},SessionCase{"Other.session",3,false}}) {
        auto fixtureDocument=document; fixtureDocument["format"]=format; fixtureDocument["version"]=version;
        if(version==1) { fixtureDocument.remove("groups"); fixtureDocument["images"]=false; }
        QFile file(path); if(!file.open(QIODevice::WriteOnly|QIODevice::Truncate)) return fail("session fixture");
        const auto bytes=QJsonDocument(fixtureDocument).toJson(); if(file.write(bytes)!=bytes.size()) return fail("write session fixture"); file.close();
        // v1 sessions carry no results, so acceptance means the explicit one-time rebuild path.
        if(loadSessionFile(path,version==1)!=expected) return fail("legacy/current/unknown session compatibility");
        QElapsedTimer wait; wait.start();
        while(scanning_ && wait.elapsed()<5000) { QApplication::processEvents(); QThread::msleep(1); }
        if(scanning_) return fail("session reconciliation timeout");
    }
    if(!screenshotPath.isEmpty() && !grab().save(screenshotPath)) return fail("window screenshot");
    qInfo("PASS branding: Compareel name, seven icon sizes, EN/TR About owner/link/logo, v1/v3 sessions accepted and unknown format rejected");
    return true;
}

bool MainWindow::runUiCheck(const QString& mediaPath,const QString& screenshotPath) {
    auto wait=[&](const std::function<bool()>& ready,int timeout=10000) { QElapsedTimer time; time.start(); while(!ready() && time.elapsed()<timeout) { QApplication::processEvents(); QThread::msleep(10); } return ready(); };
    QDir().mkpath(QFileInfo(screenshotPath).absolutePath());
    QTemporaryDir fixture(QFileInfo(screenshotPath).absolutePath()+"/ui-files-XXXXXX"); if(!fixture.isValid()) return false;
    if(!QFile::copy(mediaPath,fixture.filePath("Original.mp4")) || !QFile::copy(mediaPath,fixture.filePath("Copy.mp4"))) return false;
    folders_={QDir::toNativeSeparators(fixture.path()).toStdWString()}; options_.protectedFolders.clear(); options_.excludedFolders.clear(); profile_->setCurrentIndex(0);
    rescanAction_->trigger();
    if(!wait([&] { return !scanning_; }) || groups_.size()!=1 || groups_[0].fileIndices.size()!=2) return false;
    qInfo()<<"UI check: initial scan passed";
    rescanAction_->trigger(); pauseAction_->trigger();
    if(!wait([&] { return !scanning_; }) || !pausedScan_ || !settings_.value("resumeAvailable").toBool()) return false;
    resumeAction_->trigger(); if(!wait([&] { return !scanning_; }) || pausedScan_ || cacheHits_!=2 || groups_.size()!=1) return false;
    qInfo()<<"UI check: pause and cache resume passed";
    {
        auto checkpointPath=fixture.filePath("checkpoint.sqlite"); ScanCache reader(checkpointPath);
        CacheWriter writer(checkpointPath); writer.submit(files_[0]); writer.submit(files_[1]);
        auto a=files_[0],b=files_[1]; a.contentHash.clear(); b.contentHash.clear();
        if(!wait([&] { return reader.restore(a) && reader.restore(b); }) || a.contentHash.empty() || a.contentHash!=b.contentHash || !writer.error().isEmpty()) return false;
        qInfo()<<"UI check: file checkpoints committed before writer shutdown";
    }
    auto ignore=findChild<QAction*>("Ignore this pair"); auto reset=findChild<QAction*>("Reset ignored pairs");
    if(!ignore || !reset) return false;
    ignore->trigger(); if(!wait([&] { return !scanning_; }) || !groups_.empty()) return false;
    rescanAction_->trigger(); if(!wait([&] { return !scanning_; }) || !groups_.empty()) return false;
    reset->trigger(); rescanAction_->trigger(); if(!wait([&] { return !scanning_; }) || groups_.size()!=1) return false;
    qInfo()<<"UI check: persistent ignored pair and reset passed";
    rescanAction_->trigger();
    if(!wait([&] { return !scanning_; }) || cacheHits_!=2 || groups_.size()!=1 || !cacheError_.isEmpty()) { qWarning()<<"UI cache check failed"<<cacheHits_<<groups_.size()<<cacheError_; return false; }
    qInfo()<<"UI check: warm cache passed";
    {
        left_->setFile(nullptr,-1); right_->setFile(nullptr,-1); previewPool_.waitForDone(); QApplication::processEvents();
        left_->player->Close(); right_->player->Close();
        QFile changed(fixture.filePath("Copy.mp4")); if(!changed.open(QIODevice::Append) || changed.write("x")!=1) { qWarning()<<"Cannot modify private fixture"<<changed.errorString(); return false; } changed.close();
        rescanAction_->trigger();
        if(!wait([&] { return !scanning_; }) || cacheHits_!=1 || !groups_.empty()) { qWarning()<<"UI invalidation check failed"<<cacheHits_<<groups_.size()<<cacheError_; return false; }
        if(!changed.open(QIODevice::ReadWrite) || !changed.resize(changed.size()-1)) return false; changed.close();
        rescanAction_->trigger(); if(!wait([&] { return !scanning_; }) || groups_.size()!=1) return false;
    }
    qInfo()<<"UI check: changed-file invalidation passed";
    if(!wait([&] { return left_->player->IsLoaded() && right_->player->IsLoaded(); })) {
        qWarning()<<"UI check: players not loaded; groups"<<groups_.size()<<"current"<<currentGroup_<<"left"<<left_->fileIndex<<left_->player->IsLoaded()<<"right"<<right_->fileIndex<<right_->player->IsLoaded()<<"scanning"<<scanning_;
        return false;
    }
    {
        auto leftNameEdit=left_->findChild<QLineEdit*>("filename");
        auto leftFolderEdit=left_->findChild<QLineEdit*>("folderPath");
        auto rightNameEdit=right_->findChild<QLineEdit*>("filename");
        auto rightFolderEdit=right_->findChild<QLineEdit*>("folderPath");
        if(!leftNameEdit || !leftFolderEdit || !rightNameEdit || !rightFolderEdit) return false;
        if(!leftNameEdit->isReadOnly() || !leftFolderEdit->isReadOnly() || !rightNameEdit->isReadOnly() || !rightFolderEdit->isReadOnly()) return false;
        if(leftNameEdit->text().contains('\\') || leftNameEdit->text().contains('/') || rightNameEdit->text().contains('\\') || rightNameEdit->text().contains('/')) return false;
        // The field may show "start…end" when narrow; the full folder stays available for focus, selection and copying.
        auto fullText=[](QLineEdit* edit) { auto elided=dynamic_cast<ElidedLineEdit*>(edit); return elided?elided->fullText():edit->text(); };
        if(fullText(leftFolderEdit)!=QDir::toNativeSeparators(fixture.path()) || fullText(rightFolderEdit)!=QDir::toNativeSeparators(fixture.path())) return false;
        leftNameEdit->selectAll(); leftFolderEdit->selectAll();
        if(leftNameEdit->selectedText()!=leftNameEdit->text() || leftFolderEdit->selectedText()!=leftFolderEdit->text()) return false;
        leftNameEdit->deselect(); leftFolderEdit->deselect();
        qInfo()<<"UI check: separate selectable filename and folder fields passed";
    }
    if(results_->topLevelItemCount()!=1 || results_->topLevelItem(0)->childCount()!=0) return false;
    {
        if(groups_[0].fileIndices.size()!=2 || thumbnailRect(0,1).isEmpty()) return false;
        qInfo()<<"UI check: compact thumbnail group row passed";
    }
    {
        auto leftSlider=left_->findChild<QSlider*>("playerSeek");
        if(!leftSlider) return false;
        int64_t before=left_->player->GetPosition100ns();
        Q_EMIT leftSlider->sliderMoved(6500);
        if(!wait([&] { return std::abs(left_->player->GetPosition100ns()-before)>2000000 && left_->player->GetPosition100ns()>15000000; },3000)) {
            qWarning()<<"Individual sliderMoved live scrub failed"<<before<<left_->player->GetPosition100ns();
            return false;
        }
        leftSlider->setValue(6500);
        Q_EMIT leftSlider->sliderReleased();
        if(!wait([&] { return !left_->player->IsSeeking(); },3000)) return false;
        // Verify scrubbing all the way to 100% (EOF) does not set player error
        Q_EMIT leftSlider->sliderMoved(10000);
        leftSlider->setValue(10000);
        Q_EMIT leftSlider->sliderReleased();
        if(!wait([&] { return !left_->player->IsSeeking(); },3000) || FAILED(left_->player->GetError()) || left_->player->GetPosition100ns()<20000000) {
            qWarning()<<"Individual slider 100% EOF scrub failed"<<left_->player->GetError()<<left_->player->GetPosition100ns();
            return false;
        }
        Q_EMIT left_->findChild<QSlider*>("playerSeek")->sliderMoved(2500);
        if(!wait([&] { return left_->player->GetPosition100ns()<15000000 && right_->player->GetPosition100ns()<15000000; },3000)) {
            qWarning()<<"Shared sliderMoved live scrub failed"<<left_->player->GetPosition100ns()<<right_->player->GetPosition100ns();
            return false;
        }
        Q_EMIT left_->findChild<QSlider*>("playerSeek")->sliderMoved(7500);
        if(!wait([&] { return left_->player->GetPosition100ns()>20000000 && right_->player->GetPosition100ns()>20000000; },3000)) {
            qWarning()<<"Shared sliderMoved second live scrub failed"<<left_->player->GetPosition100ns()<<right_->player->GetPosition100ns();
            return false;
        }
        left_->findChild<QSlider*>("playerSeek")->setValue(7500);
        Q_EMIT left_->findChild<QSlider*>("playerSeek")->sliderReleased();
        if(!wait([&] { return !left_->player->IsSeeking() && !right_->player->IsSeeking(); },3000)) return false;
        seekBoth(0,true);
        if(!wait([&] { return !left_->player->IsSeeking() && !right_->player->IsSeeking(); },3000)) return false;
        qInfo()<<"UI check: live sliderMoved scrubbing and exact release passed";
    }
    playBothAction_->trigger();
    if(!wait([&] { return left_->player->GetPosition100ns()>2000000 && right_->player->GetPosition100ns()>2000000; })) return false;
    // Verify multi-step sharedSeek_ drag while playing preserves syncing_ and resumes playback on release
    Q_EMIT left_->findChild<QSlider*>("playerSeek")->sliderMoved(3000);
    Q_EMIT left_->findChild<QSlider*>("playerSeek")->sliderMoved(4000);
    left_->findChild<QSlider*>("playerSeek")->setValue(4000);
    Q_EMIT left_->findChild<QSlider*>("playerSeek")->sliderReleased();
    if(!wait([&] { return left_->player->IsPlaying() && right_->player->IsPlaying() && left_->player->GetPosition100ns()>10000000; },4000)) {
        qWarning()<<"Shared slider drag while playing did not resume playback";
        return false;
    }
    playBothAction_->trigger();
    if(std::abs(left_->player->GetPosition100ns()-right_->player->GetPosition100ns())>1400000) return false;
    seekBoth(13500000);
    if(!wait([&] { return !left_->player->IsSeeking() && !right_->player->IsSeeking(); })) return false;
    if(left_->player->GetPosition100ns()<13500000 || right_->player->GetPosition100ns()<13500000) return false;
    wait([&] { return files_[0].frameLoaded && files_[1].frameLoaded; });
    populateResults(); tick(); QApplication::processEvents();
    if(!grab().save(screenshotPath)) return false;
    for(auto name:{"Wipe","Overlay","Difference (display aid)","Side by side"}) {
        auto mode=findChild<QAction*>(name); if(!mode) return false; mode->trigger(); QApplication::processEvents();
        auto rendered=right_->canvas->grab(); if(rendered.isNull()) return false;
        if(QString(name)=="Difference (display aid)") {
            auto image=rendered.toImage(); auto center=image.pixelColor(image.width()/2,image.height()/2);
            if(center.red()>2 || center.green()>2 || center.blue()>2) { qWarning()<<"Difference pixel mismatch"<<center<<left_->player->GetPosition100ns()<<right_->player->GetPosition100ns(); rendered.save(screenshotPath+".difference-failed.png"); return false; }
            if(!rendered.save(screenshotPath+".difference.png")) return false;
        }
    }
    qInfo()<<"UI check: comparison view actions rendered";
    offset_->setValue(0.5); seekBoth(10000000);
    if(!wait([&] { return !left_->player->IsSeeking() && !right_->player->IsSeeking(); })) return false;
    if(std::abs(right_->player->GetPosition100ns()-left_->player->GetPosition100ns()-5000000)>700000) return false;
    offset_->setValue(0); seekBoth(13500000);
    if(!wait([&] { return !left_->player->IsSeeking() && !right_->player->IsSeeking(); })) return false;
    qInfo()<<"UI check: manual time offset passed";
    auto sessionPath=fixture.filePath("review.cvsession");
    offset_->setValue(0.5);
    if(!wait([&] { return !right_->player->IsSeeking(); })) return false;
    findChild<QAction*>("Overlay")->trigger();
    if(!saveSessionFile(sessionPath)) return false;
    {
        QFile session(sessionPath); if(!session.open(QIODevice::ReadOnly)) return false;
        auto object=QJsonDocument::fromJson(session.readAll()).object(); session.close();
        auto records=object["files"].toArray(); if(records.size()!=2) return false;
        auto duplicate=records[1].toObject(); duplicate["identity"]=records[0].toObject()["identity"]; records[1]=duplicate; object["files"]=records;
        auto invalidPath=fixture.filePath("invalid.cvsession"); QFile invalid(invalidPath); if(!invalid.open(QIODevice::WriteOnly)) return false;
        invalid.write(QJsonDocument(object).toJson()); invalid.close();
        if(loadSessionFile(invalidPath) || groups_.size()!=1) return false;
        qInfo()<<"UI check: duplicate physical identity session rejected";
    }
    if(!loadSessionFile(sessionPath)) { qWarning()<<"Session save/load rejected"; return false; }
    if(!wait([&] { return !scanning_ && !restorePositions_ && left_->player->IsLoaded() && right_->player->IsLoaded() && !left_->player->IsSeeking() && !right_->player->IsSeeking(); })) { qWarning()<<"Session players did not resume"; return false; }
    if(groups_.size()!=1 || left_->player->GetPosition100ns()<13500000 || right_->player->GetPosition100ns()<18500000 || alignmentOffset100ns_!=5000000 || right_->canvas->comparisonMode()!=2) { qWarning()<<"Session position mismatch"<<groups_.size()<<left_->player->GetPosition100ns()<<right_->player->GetPosition100ns(); return false; }
    findChild<QAction*>("Side by side")->trigger();
    qInfo()<<"UI check: session positions restored";
    auto trash=findChild<QToolButton*>("trashRight"); if(!trash || !trash->isEnabled()) return false;
    auto markAction=findChild<QAction*>("Preview cleanup marks…"); if(!markAction || !markAction->isEnabled()) return false;
    auto markDialogPassed=std::make_shared<bool>(false);
    QTimer::singleShot(0,this,[markDialogPassed] {
        auto dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget()); if(!dialog) return;
        auto tree=dialog->findChild<QTreeWidget*>(); auto buttons=dialog->findChild<QDialogButtonBox*>();
        if(tree && buttons && tree->topLevelItemCount()==1 && tree->topLevelItem(0)->childCount()==1) {
            auto row=tree->topLevelItem(0)->child(0); *markDialogPassed=!row->isDisabled() && row->checkState(0)==Qt::Checked;
            // The whole window from the screen, title bar included, to review the title and the button order.
            // It is taken once the window has been on screen for a moment, so the system has drawn it.
            if(const auto shots=qEnvironmentVariable("COMPAREEL_CHECK_SHOTS"); !shots.isEmpty() && dialog->screen()) {
                QTimer::singleShot(500,dialog,[dialog,buttons,shots] {
                    const auto frame=dialog->frameGeometry(), screen=dialog->screen()->geometry();
                    dialog->screen()->grabWindow(0,frame.x()-screen.x(),frame.y()-screen.y(),frame.width(),frame.height()).save(shots+"/cleanup-marks-preview.png");
                    buttons->button(QDialogButtonBox::Ok)->click();
                });
            } else buttons->button(QDialogButtonBox::Ok)->click();
        } else dialog->reject();
    });
    markAction->trigger();
    if(!*markDialogPassed || markedFiles_.size()!=1 || markedFiles_.contains(left_->fileIndex) || !markedFiles_.contains(right_->fileIndex)) return false;
    qInfo()<<"UI check: preview marks preserved reference and marked candidate";
    auto csvPath=fixture.filePath("results.csv"); if(!exportResultsFile(csvPath)) return false;
    QFile csv(csvPath); if(!csv.open(QIODevice::ReadOnly)) return false;
    auto exported=csv.readAll(); csv.close();
    if(!exported.startsWith(QByteArray::fromHex("efbbbf")) || exported.count('\n')!=3 || !exported.contains("sha256")) return false;
    auto removedPath=QString::fromStdWString(files_[right_->fileIndex].fullPath);
    trash->click();
    if(!wait([&] { return !fileOperationPending_; })) return false;
    { ScanCache cache(cachePath_); auto history=cache.operationHistory(); if(!cache.error().isEmpty() || std::none_of(history.begin(),history.end(),[&](const auto& row) { return row.contains(removedPath) && row.contains("Completed"); })) return false; }
    qInfo()<<"UI check: CSV export and actual recycle history passed";
    {
        ScanCache cache(cachePath_); auto historyCount=cache.operationHistory().size();
        cache.ignorePair(files_[0],files_[1]); auto ignored=cache.ignoredPairs();
        cache.clearScanData(); auto cached=files_[0];
        if(!cache.error().isEmpty() || cache.restore(cached) || cache.ignoredPairs()!=ignored || cache.operationHistory().size()!=historyCount) return false;
        cache.clearIgnoredPairs(); qInfo()<<"UI check: cache clear preserved decisions and real operation history";
    }
    // Check the real files rather than relying on selection state after auto-advance.
    int remaining=QDir(fixture.path()).entryList({"*.mp4"},QDir::Files).size();
    left_->player->Close(); right_->player->Close();
    if(remaining!=1 || !groups_.empty()) { qWarning()<<"UI check: after recycle remaining"<<remaining<<"groups"<<groups_.size(); return false; }
    {
        // F3 & F5: 3-file group A/B/C sort-order #1 change, right-candidate selection, session persistence, and deleting C while keeping B untouched.
        QDir().mkpath(fixture.filePath("SubZ"));
        QString pathB=fixture.filePath("SubZ/A_First.mp4");
        QString pathC=fixture.filePath("Z_Candidate_C.mp4");
        if(!QFile::copy(mediaPath,pathB) || !QFile::copy(mediaPath,pathC)) { qWarning()<<"UI check: A/B/C fixture copies"; return false; }
        rescanAction_->trigger();
        if(!wait([&] { return !scanning_; }) || groups_.size()!=1 || groups_[0].fileIndices.size()!=3) { qWarning()<<"UI check: three-file rescan groups"<<groups_.size()<<"scanning"<<scanning_; return false; }
        if(results_->topLevelItemCount()!=1 || results_->topLevelItem(0)->childCount()!=0) { qWarning()<<"UI check: one compact A/B/C row"; return false; }
        if(thumbnailRect(0,2).isEmpty()) { qWarning()<<"UI check: three thumbnails"; return false; }
        // The reference rule picks the left file; sorting only orders groups, so changing it must leave the reference alone.
        const QString fileA="A_First.mp4";
        auto referenceIs=[&] { return QString::fromStdWString(files_[groups_[0].fileIndices[0]].fileName)==fileA && QString::fromStdWString(files_[left_->fileIndex].fileName)==fileA; };
        referenceRule_->setCurrentIndex(referenceRule_->findData(int(ReferenceRule::NameOrder)));
        sort_->setCurrentIndex(static_cast<int>(SortOrder::NameAsc));
        QApplication::processEvents();
        if(!referenceIs()) { qWarning()<<"UI check: name reference rule"; return false; }
        auto sortSession=fixture.filePath("sort-name.cvsession");
        if(!saveSessionFile(sortSession)) { qWarning()<<"UI check: A/B/C step failed at line"<<__LINE__; return false; }
        sort_->setCurrentIndex(static_cast<int>(SortOrder::SizeDesc));
        QApplication::processEvents();
        if(!referenceIs()) { qWarning()<<"UI check: reference kept across sort change"; return false; }
        if(!loadSessionFile(sortSession) || !wait([&] { return !scanning_; })) { qWarning()<<"UI check: A/B/C step failed at line"<<__LINE__; return false; }
        if(sort_->currentIndex()!=static_cast<int>(SortOrder::NameAsc) || referenceRule_->currentData().toInt()!=int(ReferenceRule::NameOrder) || !referenceIs()) { qWarning()<<"UI check: session sort/reference round trip"; return false; }
        sort_->setCurrentIndex(static_cast<int>(SortOrder::SizeDesc));
        QApplication::processEvents();
        if(!referenceIs()) { qWarning()<<"UI check: A/B/C step failed at line"<<__LINE__; return false; }
        if(groups_[0].fileIndices.size()!=3) { qWarning()<<"UI check: A/B/C step failed at line"<<__LINE__; return false; }
        clickThumbnail(0,2);
        if(!wait([&] { return groups_[0].rightSlot==2; },1000)) { qWarning()<<"UI check: A/B/C step failed at line"<<__LINE__; return false; }
        clickThumbnail(0,1);
        if(!wait([&] { return groups_[0].rightSlot==1; },1000)) { qWarning()<<"UI check: A/B/C step failed at line"<<__LINE__; return false; }
        clickThumbnail(0,2);
        if(!wait([&] { return groups_[0].rightSlot==2; },1000)) { qWarning()<<"UI check: A/B/C step failed at line"<<__LINE__; return false; }
        if(groups_[0].rightSlot!=2 || QString::fromStdWString(files_[left_->fileIndex].fileName)!=fileA || QString::fromStdWString(files_[right_->fileIndex].fileName)!="Z_Candidate_C.mp4") { qWarning()<<"UI check: A/B/C step failed at line"<<__LINE__; return false; }
        if(results_->topLevelItem(0)->data(0,Qt::UserRole+1).toInt()!=2) { qWarning()<<"UI check: A/B/C step failed at line"<<__LINE__; return false; }
        if(!wait([&] { return left_->player->IsLoaded() && right_->player->IsLoaded(); })) { qWarning()<<"UI check: A/B/C step failed at line"<<__LINE__; return false; }
        wait([&] { return std::all_of(files_.begin(),files_.end(),[](const auto& f) { return f.deleted || f.frameLoaded; }); });
        populateResults(); tick(); QApplication::processEvents();
        // The manual test loop must deliver deferred widget deletion like app.exec().
        QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
        if(groups_[0].fileIndices.size()!=3 || !thumbnailView(0,2).selected || thumbnailView(0,1).selected) { qWarning()<<"UI check: A/B/C step failed at line"<<__LINE__; return false; }
        if(!grab().save(screenshotPath)) { qWarning()<<"UI check: A/B/C step failed at line"<<__LINE__; return false; }
        auto abcSession=fixture.filePath("abc.cvsession");
        if(!saveSessionFile(abcSession)) { qWarning()<<"UI check: A/B/C step failed at line"<<__LINE__; return false; }
        selectGroup(0,1);
        if(groups_[0].rightSlot!=1) { qWarning()<<"UI check: A/B/C step failed at line"<<__LINE__; return false; }
        if(!loadSessionFile(abcSession) || !wait([&] { return !scanning_ && left_->player->IsLoaded() && right_->player->IsLoaded(); })) { qWarning()<<"UI check: A/B/C step failed at line"<<__LINE__; return false; }
        if(groups_.size()!=1 || groups_[0].rightSlot!=2 || QString::fromStdWString(files_[right_->fileIndex].fileName)!="Z_Candidate_C.mp4") { qWarning()<<"UI check: A/B/C step failed at line"<<__LINE__; return false; }
        trash->click();
        if(!wait([&] { return !fileOperationPending_; })) { qWarning()<<"UI check: A/B/C step failed at line"<<__LINE__; return false; }
        if(QFileInfo::exists(pathC) || !QFileInfo::exists(pathB)) { qWarning()<<"UI check: A/B/C step failed at line"<<__LINE__; return false; }
        { ScanCache cache(cachePath_); if(!cache.ignoredPairs().empty()) { qWarning()<<"UI check: A/B/C step failed at line"<<__LINE__; return false; } }
        if(groups_.size()!=1 || groups_[0].fileIndices.size()!=2) { qWarning()<<"UI check: A/B/C step failed at line"<<__LINE__; return false; }
        // C is gone: the reference A stays on the left and the remaining original copy is the candidate.
        if(const auto right=QString::fromStdWString(files_[right_->fileIndex].fileName); QString::fromStdWString(files_[left_->fileIndex].fileName)!=fileA || right==fileA || right=="Z_Candidate_C.mp4") { qWarning()<<"UI check: pair after deleting C"; return false; }
        trash->click();
        if(!wait([&] { return !fileOperationPending_; })) { qWarning()<<"UI check: A/B/C step failed at line"<<__LINE__; return false; }
        left_->player->Close(); right_->player->Close();
        // Removing the candidate leaves exactly one copy in the scanned tree: the reference A in SubZ.
        if(QDir(fixture.path()).entryList({"*.mp4"},QDir::Files).size()+QDir(fixture.filePath("SubZ")).entryList({"*.mp4"},QDir::Files).size()!=1 || !QFileInfo::exists(pathB) || !groups_.empty()) { qWarning()<<"UI check: A/B/C step failed at line"<<__LINE__; return false; }
        qInfo()<<"UI check: 3-file A/B/C sort-order #1 update, right-candidate selection, session restore, and C deletion passed";
    }
    for(int count:{4,20}) {
        for(int i=1;i<count;++i) if(!QFile::copy(mediaPath,fixture.filePath(QString("Batch-%1-%2.mp4").arg(count).arg(i)))) return false;
        rescanAction_->trigger();
        if(!wait([&] { return !scanning_; }) || groups_.size()!=1 || groups_[0].fileIndices.size()!=count) return false;
        if(results_->topLevelItem(0)->childCount()!=0 || groups_[0].fileIndices.size()!=size_t(count) || thumbnailRect(0,count-1).isEmpty()) return false;
        auto anchor=files_[groups_[0].fileIndices[0]].fullPath;
        if(count==20) {
            if(!wait([&] { return rowScrollMax(0)>0; })) { qWarning()<<"Overflow row cannot scroll"; return false; }
            // A sideways wheel over the row scrolls it to the end, like the scroller and the arrow keys.
            const auto rowRect=results_->visualItemRect(results_->topLevelItem(0));
            for(int step=0;step<40 && rowScroll(0)<rowScrollMax(0);++step) {
                QWheelEvent wheel(QPointF(rowRect.center()),QPointF(results_->viewport()->mapToGlobal(rowRect.center())),QPoint(),QPoint(-120,0),Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);
                QApplication::sendEvent(results_->viewport(),&wheel);
            }
            if(rowScroll(0)!=rowScrollMax(0)) { qWarning()<<"Overflow wheel scroll"<<rowScroll(0)<<rowScrollMax(0); return false; }
            int last=groups_[0].fileIndices.back();
            if(!wait([&] { return files_[last].frameLoaded; })) {
                qWarning()<<"Overflow thumbnail did not load"<<rowScroll(0)<<rowScrollMax(0)<<pendingPreviews_.size();
                grab().save(screenshotPath+".overflow.png"); return false;
            }
            if(rowScroll(0)==0) { qWarning()<<"Overflow scroll was reset"; return false; }
            clickThumbnail(0,19);
            if(!wait([&] { return groups_[0].rightSlot==19 && right_->fileIndex==last; })) { qWarning()<<"Overflow last selection failed"; return false; }
            clickThumbnail(0,0);
            QApplication::processEvents();
            if(groups_[0].rightSlot!=19 || files_[left_->fileIndex].fullPath!=anchor) return false;
            qInfo()<<"UI check: 20 thumbnails, horizontal scrolling, direct last-candidate click, fixed left passed";
        }
        if(count==4) {
            auto leftTrash=findChild<QToolButton*>("trashLeft"); if(!leftTrash || !leftTrash->isEnabled()) return false;
            leftTrash->click();
            if(!wait([&] { return !fileOperationPending_; })) return false;
            if(groups_.size()!=1 || groups_[0].fileIndices.size()!=3 || QFileInfo::exists(QString::fromStdWString(anchor))) return false;
            anchor=files_[groups_[0].fileIndices[0]].fullPath;
        }
        while(!groups_.empty()) {
            if(files_[groups_[0].fileIndices[0]].fullPath!=anchor) return false;
            trash->click();
            if(!wait([&] { return !fileOperationPending_; })) return false; QApplication::processEvents();
        }
        if(!QFileInfo::exists(QString::fromStdWString(anchor)) || QDir(fixture.path()).entryList({"*.mp4"},QDir::Files).size()!=1) return false;
    }
    auto survivor=QDir(fixture.path()).entryList({"*.mp4"},QDir::Files).first();
    QString extra=fixture.filePath("Manual comparison.mp4"); if(!QFile::copy(mediaPath,extra)) return false;
    openFiles({fixture.filePath(survivor),extra});
    if(!manual_ || !wait([&] { return left_->player->IsLoaded() && right_->player->IsLoaded(); })) return false;
    toggleProtection(right_); if(!files_[right_->fileIndex].protectedFile || trash->isEnabled()) return false;
    toggleProtection(right_); if(!trash->isEnabled()) return false;
    {
        auto pair=ReviewPairKey(files_[left_->fileIndex].identity,files_[right_->fileIndex].identity);
        { ScanCache cache(cachePath_); cache.ignorePair(files_[left_->fileIndex],files_[right_->fileIndex]); }
        auto decisionsPath=fixture.filePath("decisions.cvsession"); if(!saveSessionFile(decisionsPath)) return false;
        { ScanCache cache(cachePath_); cache.clearIgnoredPairs(); }
        if(!loadSessionFile(decisionsPath) || !wait([&] { return !scanning_; })) return false;
        { ScanCache cache(cachePath_); if(!cache.ignoredPairs().contains(pair)) return false; cache.clearIgnoredPairs(); }
        qInfo()<<"UI check: review decisions restored from session";
    }
    options_.protectedFolders={QDir::toNativeSeparators(fixture.path()).toStdWString()};
    rescanAction_->trigger(); if(!wait([&] { return !scanning_; }) || groups_.size()!=1) return false;
    toggleProtection(right_); if(!files_[right_->fileIndex].protectedByFolder || !files_[right_->fileIndex].protectedFile || trash->isEnabled()) return false;
    left_->player->Close(); right_->player->Close();
    if(!saveSessionFile(sessionPath)) return false;
    previewPool_.waitForDone(); QApplication::processEvents();
    QFile changed(extra); if(!changed.open(QIODevice::Append) || changed.write("x")!=1) return false; changed.close();
    if(!loadSessionFile(sessionPath) || !wait([&] { return !scanning_; }) || !groups_.empty() || scanIssues_.empty()) return false;
    qInfo()<<"UI check: session restore and changed-file rejection passed";
    options_.protectedFolders.clear();
    openFiles({fixture.filePath(survivor),extra});
    if(!wait([&] { return left_->player->IsLoaded() && right_->player->IsLoaded(); })) return false;
    trash->click(); if(!fileOperationPending_) return false;
    QElapsedTimer closeTime; closeTime.start(); close();
    if((!closing_ && !pendingClose_) || closeTime.elapsed()>100) return false;
    if(!wait([&] { return closeReady_ && !isVisible(); })) return false;
    if(QDir(fixture.path()).entryList({"*.mp4"},QDir::Files).size()!=1) return false;
    qInfo()<<"UI check: asynchronous reader shutdown passed";
    return true;
}

// Times the per-pair review actions on a long result list, including the event-loop work they queue.
bool MainWindow::runLatencyCheck(const QString& mediaPath) {
    auto fail=[](const char* message) { qCritical("FAIL latency: %s",message); return false; };
    QTemporaryDir fixture; if(!fixture.isValid()) return fail("private root");
    constexpr int groupCount=150,perGroup=3;
    files_.clear(); groups_.clear(); markedFiles_.clear(); currentGroup_=-1;
    for(int g=0;g<groupCount;++g) {
        VideoGroup group; group.rightSlot=1;
        for(int slot=0;slot<perGroup;++slot) {
            const auto path=QDir::toNativeSeparators(fixture.filePath(QString("g%1_%2.mp4").arg(g).arg(slot)));
            if(!mediaPath.isEmpty()) { if(!QFile::copy(mediaPath,path)) return fail("private media copies"); }
            else { QFile file(path); if(!file.open(QIODevice::WriteOnly) || file.write(QByteArray(64+g,'x'))<0) return fail("private files"); }
            VideoFileInfo file; file.fullPath=path.toStdWString(); file.folderPath=QDir::toNativeSeparators(fixture.path()).toStdWString();
            file.fileName=QFileInfo(path).fileName().toStdWString(); file.fileSize=QFileInfo(path).size(); file.metadataLoaded=true;
            file.contentHash="group"+std::to_string(g); file.identity=FileOperations::ReadIdentity(file.fullPath);
            if(!file.identity.valid) return fail("file identity");
            group.fileIndices.push_back(int(files_.size())); files_.push_back(std::move(file));
        }
        groups_.push_back(std::move(group));
    }
    const auto savedCriteria=scanCriteria_; const bool savedManual=manual_;
    scanCriteria_=ScanCriteria{}; scanCriteria_.matchContent=true; manual_=false;
    auto measure=[](auto&& work) { QElapsedTimer timer; timer.start(); work(); QApplication::processEvents(); return timer.nsecsElapsed()/1e6; };
    const double populate=measure([this] { populateResults(); });
    double navigationMax=0,navigationTotal=0;
    for(int g=0;g<30;++g) { const double ms=measure([this,g] { selectGroup(g); }); navigationMax=std::max(navigationMax,ms); navigationTotal+=ms; }
    const double candidate=measure([this] { selectGroup(5,2); });
    selectGroup(10);
    // Decisions are toggles: the same button sets and then clears the pair decision.
    const double keep=measure([this] { reviewActions_[2]->trigger(); });
    if(currentReviewDecision()!=ReviewDecision::KeepBoth || !reviewActions_[2]->isChecked() || !reviewActions_[0]->isEnabled()) return fail("decision set");
    const double clear=measure([this] { reviewActions_[2]->trigger(); });
    if(currentReviewDecision()!=ReviewDecision::Unreviewed || reviewActions_[2]->isChecked() || reviewActions_[0]->isEnabled()) return fail("pressing the active decision again clears it");
    const double mark=measure([this] { toggleRightMark(); });
    const double unmark=measure([this] { toggleRightMark(); });
    const double ignore=measure([this] { ignoreCurrentPair(); });
    { ScanCache cache(cachePath_); cache.clearIgnoredPairs(); cache.clearReviewDecisions(); }
    left_->setFile(nullptr,-1); right_->setFile(nullptr,-1); files_.clear(); groups_.clear(); markedFiles_.clear(); currentGroup_=-1; populateResults();
    scanCriteria_=savedCriteria; manual_=savedManual;
    qInfo().noquote()<<QString("LATENCY groups=%1 media=%2 populate_ms=%3 navigate_avg_ms=%4 navigate_max_ms=%5 candidate_ms=%6 keep_ms=%7 clear_ms=%8 mark_ms=%9 unmark_ms=%10 ignore_ms=%11")
        .arg(groupCount).arg(mediaPath.isEmpty()?"no":"yes").arg(populate,0,'f',1).arg(navigationTotal/30,0,'f',1).arg(navigationMax,0,'f',1).arg(candidate,0,'f',1)
        .arg(keep,0,'f',1).arg(clear,0,'f',1).arg(mark,0,'f',1).arg(unmark,0,'f',1).arg(ignore,0,'f',1);
    // Generous ceilings: each action touches one or two rows, so none may grow with the result count.
    if(navigationMax>250 || candidate>250 || keep>250 || clear>250 || mark>250 || unmark>250 || ignore>250) return fail("a review action exceeded 250 ms");
    return true;
}

// Themes, languages and the Settings window: every theme is applied live and photographed on the
// welcome card, the Settings window (English and Turkish) and a loaded comparison with state badges.
bool MainWindow::runAppearanceCheck(const QString& mediaPath,const QString& screenshotPath) {
    auto wait=[](const std::function<bool()>& ready) {
        QElapsedTimer elapsed; elapsed.start();
        while(!ready() && elapsed.elapsed()<20000) { QApplication::processEvents(); QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete); QThread::msleep(10); }
        return ready();
    };
    auto settle=[] { for(int i=0;i<10;++i) { QApplication::processEvents(); QThread::msleep(20); } };
    auto fail=[](const char* what) { qCritical("FAIL appearance: %s",what); return false; };
    const auto shots=QFileInfo(screenshotPath).absolutePath()+"/"+QFileInfo(screenshotPath).completeBaseName();
    QDir().mkpath(QFileInfo(screenshotPath).absolutePath());
    // Menus: the general settings live in one window, reached from Tools.
    for(auto menu:menuBar()->findChildren<QMenu*>(QString(),Qt::FindDirectChildrenOnly)) if(menu->objectName()=="Settings") return fail("Settings menu still in the menu bar");
    if(!findChild<QAction*>("Settings…") || !findChild<QAction*>("Scan rules…") || findChild<QAction*>("Add Compareel to Send To")) return fail("settings actions");
    // Languages come from files: Turkish is bundled, and an unknown code falls back to English.
    bool turkish=false; for(const auto& info:Language::available()) turkish=turkish || (info.code=="tr" && info.name==QString::fromUtf8("Türkçe"));
    if(!turkish) return fail("bundled Turkish language file");
    if(Language::setCurrent("xx") || Language::current()!="en" || text("Settings")!="Settings") return fail("unknown language fallback");
    if(!Language::setCurrent("tr") || text("Settings")!="Ayarlar") return fail("Turkish lookup");
    Language::setCurrent("en"); translate();
    for(const auto& theme:Theme::themes()) {
        setTheme(theme.id); settle();
        if(qApp->palette().color(QPalette::Window)!=Theme::color("@surface") || settings_.value("theme").toString()!=theme.id) return fail("theme palette or persistence");
        if(!grab().save(shots+"-welcome-"+theme.id+".png")) return fail("welcome screenshot");
    }
    // The Settings window: switch theme and language from inside it, as a user would.
    bool shown=false; QString failure;
    QTimer::singleShot(200,this,[&] {
        auto dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if(!dialog || dialog->objectName()!="settingsDialog") { failure="settings dialog"; return; }
        auto theme=dialog->findChild<QComboBox*>("settingsTheme"); auto language=dialog->findChild<QComboBox*>("settingsLanguage");
        if(!theme || !language || theme->count()!=int(Theme::themes().size()) || language->findData("tr")<0) { failure="settings choices"; dialog->reject(); return; }
        for(const char* code:{"en","tr"}) {
            language->setCurrentIndex(language->findData(code)); emit language->activated(language->currentIndex());
            if(Language::current()!=code || dialog->windowTitle()!=text("Settings")) { failure="language switch in settings"; break; }
            for(int i=0;i<theme->count();++i) {
                theme->setCurrentIndex(i); emit theme->activated(i);
                for(int k=0;k<10;++k) { QApplication::processEvents(); QThread::msleep(20); }
                if(Theme::current()!=theme->currentData().toString()) { failure="theme switch in settings"; break; }
                dialog->adjustSize(); QApplication::processEvents();
                dialog->grab().save(shots+"-settings-"+Theme::current()+"-"+code+".png");
            }
        }
        shown=true; dialog->reject();
    });
    findChild<QAction*>("Settings…")->trigger();
    if(!failure.isEmpty()) return fail(failure.toUtf8().constData());
    if(!shown) return fail("settings dialog not shown");
    Language::setCurrent("en"); translate();
    // A loaded comparison: three files in one group plus a second group, with a decision and a cleanup mark.
    QTemporaryDir fixture(QFileInfo(screenshotPath).absolutePath()+"/appearance-files-XXXXXX");
    if(!fixture.isValid()) return fail("fixture directory");
    for(const auto& name:{"A0_reference.mp4","A1_copy.mp4","A2_copy.mp4","B0_second.mp4","B1_second.mp4"}) {
        const auto path=fixture.filePath(name); if(!QFile::copy(mediaPath,path)) return fail("fixture copy");
        if(QString(name).startsWith('B')) { QFile file(path); if(!file.open(QIODevice::Append) || file.write("x")!=1) return fail("fixture mutation"); }
    }
    { QSignalBlocker a(hideVideosAction_),b(hideThumbnailsAction_); hideVideosAction_->setChecked(false); hideThumbnailsAction_->setChecked(false); }
    applyPrivacy(); profile_->setCurrentIndex(0); sort_->setCurrentIndex(int(SortOrder::NameAsc));
    folders_={QDir::toNativeSeparators(fixture.path()).toStdWString()}; options_.minFileSize=1; options_.protectedFolders.clear(); options_.excludedFolders.clear();
    startScan();
    if(!wait([&] { return !scanning_ && left_->player->IsLoaded() && right_->player->IsLoaded() && pendingPreviews_.empty(); }) || groups_.size()!=2) return fail("scan/load");
    selectGroup(0,2); toggleRightMark(); selectGroup(0,1); setReviewDecision(ReviewDecision::KeepBoth);
    if(!wait([&] { return pendingPreviews_.empty(); })) return fail("previews");
    for(const auto& theme:Theme::themes()) {
        setTheme(theme.id); settle();
        if(!grab().save(shots+"-main-"+theme.id+".png")) return fail("main screenshot");
    }
    setTheme("dark"); settle();
    // The compared group changes only on a click or an arrow key, never because Qt moved the current row.
    {
        selectGroup(0,1); settle();
        results_->setCurrentItem(results_->topLevelItem(1)); settle();
        if(currentGroup_!=0 || results_->currentItem()!=results_->topLevelItem(0)) return fail("current-row change switched the group");
        if(thumbnailRect(1,0).isEmpty()) return fail("second row thumbnail");
        { const QPointF over(thumbnailRect(1,0).center()); QMouseEvent move(QEvent::MouseMove,over,results_->viewport()->mapToGlobal(over),Qt::NoButton,Qt::NoButton,Qt::NoModifier);
          QApplication::sendEvent(results_->viewport(),&move); settle(); }
        if(currentGroup_!=0) return fail("hovering a thumbnail switched the group");
        const auto rect=results_->visualItemRect(results_->topLevelItem(1));
        QMouseEvent press(QEvent::MouseButtonPress,QPointF(rect.right()-30,rect.top()+8),results_->viewport()->mapToGlobal(QPointF(rect.right()-30,rect.top()+8)),Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease,press.position(),press.globalPosition(),Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
        QApplication::sendEvent(results_->viewport(),&press); QApplication::sendEvent(results_->viewport(),&release); settle();
        if(currentGroup_!=1) return fail("clicking a group row did not select it");
        selectGroup(0,1); settle();
    }
    // Text buttons get their full height, so descenders such as the y in "Ayrıntılar" are not cut.
    for(auto details:findChildren<QToolButton*>("fileDetails")) if(details->height()<details->sizeHint().height()) return fail("details button clipped");
    // Lists drop down below the box whichever item is selected, as Windows combo boxes do.
    sort_->setCurrentIndex(sort_->count()-1); sort_->showPopup(); settle();
    auto popup=sort_->view()?sort_->view()->window():nullptr;
    const bool below=popup && popup->isVisible() && popup->geometry().top()>=sort_->mapToGlobal(QPoint(0,sort_->height())).y()-2;
    if(popup && screen()) screen()->grabWindow(0).copy(geometry().united(popup->geometry()).translated(-screen()->geometry().topLeft())).save(shots+"-combo-popup.png");
    sort_->hidePopup(); settle(); sort_->setCurrentIndex(int(SortOrder::NameAsc));
    if(!below) return fail("combo popup opens over or above the box");
    // Operation history can be cleared; an interrupted entry survives because its outcome still needs review.
    {
        ScanCache cache(cachePath_); const auto& file=files_[groups_[0].fileIndices[0]];
        cache.recordRemoval(file,S_OK); const auto pending=cache.beginRemoval(file);
        if(!cache.error().isEmpty() || pending<=0 || cache.operationHistory().size()<2) return fail("history fixture");
    }
    bool cleared=false; failure.clear();
    QTimer::singleShot(200,this,[&] {
        auto dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());
        auto clear=dialog?dialog->findChild<QPushButton*>("clearOperationHistory"):nullptr;
        auto rows=dialog?dialog->findChild<QTreeWidget*>("operationHistoryTable"):nullptr;
        if(!clear || !clear->isEnabled() || !rows || rows->topLevelItemCount()<2 || rows->topLevelItem(0)->text(0).contains('T')) { failure="history table or clear button"; if(dialog) dialog->reject(); return; }
        dialog->grab().save(shots+"-history.png");
        QTimer::singleShot(200,this,[&] { if(auto box=qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) box->button(QMessageBox::Yes)->click(); });
        clear->click();
        auto table=dialog->findChild<QTreeWidget*>("operationHistoryTable"); dialog->grab().save(shots+"-history-cleared.png");
        cleared=table && table->topLevelItemCount()==1 && table->topLevelItem(0)->text(2)==text("Outcome unconfirmed after interruption; rescan before retry");
        dialog->reject();
    });
    findChild<QAction*>("Operation history")->trigger();
    if(!failure.isEmpty()) return fail(failure.toUtf8().constData());
    if(!cleared) return fail("history not cleared, or the interrupted entry was removed");
    setTheme("dark"); setReviewDecision(ReviewDecision::Unreviewed); markedFiles_.clear(); populateResults();
    qInfo()<<"PASS appearance: 4 live themes persisted, Settings window switches theme and EN/TR, file languages with English fallback, menus regrouped; screenshots at"<<shots;
    return true;
}
