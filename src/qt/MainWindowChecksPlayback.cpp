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

bool MainWindow::runSubtitleUiCheck(const QString& mediaPath,const QString& screenshotPath) {
    Language::setCurrent("en"); translate();
    QTemporaryDir fixture; if(!fixture.isValid()) return false;
    const auto left=fixture.filePath("left.mp4"),right=fixture.filePath("right.mp4");
    if(!QFile::copy(mediaPath,left) || !QFile::copy(mediaPath,right)) return false;
    openFiles({left,right});
    QElapsedTimer deadline; deadline.start();
    while((!left_->player->IsLoaded() || !right_->player->IsLoaded()) && deadline.elapsed()<10000) { QApplication::processEvents(); QThread::msleep(10); }
    if(!left_->player->IsLoaded() || !right_->player->IsLoaded()) return false;
    bool inspected=false; QTimer inspect; inspect.setInterval(20); deadline.restart();
    connect(&inspect,&QTimer::timeout,this,[&] {
        if(auto dialog=findChild<QDialog*>("fileDetailsDialog")) {
            auto label=dialog->findChild<QLabel*>("detailSubtitles");
            auto fps=dialog->findChild<QLabel*>("detailFps"),folder=dialog->findChild<QLabel*>("detailFolder"),resolution=dialog->findChild<QLabel*>("detailResolution");
            inspected=label && label->text().contains("1 embedded track") && label->text().contains("mov_text") && fps && fps->text().contains("12.000") && folder && QDir::fromNativeSeparators(folder->text())==fixture.path() && resolution && resolution->text()=="320 × 180";
            if(inspected || deadline.elapsed()>7000) { dialog->grab().save(screenshotPath); dialog->accept(); }
        }
    });
    inspect.start(); left_->findChild<QToolButton*>("fileDetails")->click(); inspect.stop();
    left_->player->Close(); right_->player->Close();
    if(inspected) qInfo()<<"PASS subtitles UI: actual details dialog asynchronously displays embedded mov_text track via bundled FFmpeg";
    else qWarning()<<"Subtitle UI inspection failed";
    return inspected;
}

bool MainWindow::runPlaybackCheck(const QString& mediaPath,const QString& screenshotPath,int durationSeconds) {
    auto wait=[](const std::function<bool()>& ready) { QElapsedTimer t; t.start(); while(!ready() && t.elapsed()<15000) { QApplication::processEvents(); QThread::msleep(10); } return ready(); };
    auto fail=[](const char* reason) { qWarning()<<"Playback check failed:"<<reason; return false; };

    openFiles({mediaPath,QFileInfo(mediaPath).dir().filePath("vfr.mp4")}); if(!wait([&] { return left_->player->IsLoaded() && right_->player->IsLoaded(); })) return fail("load CFR/VFR pair");
    left_->player->SetMuted(false); right_->player->SetMuted(false);
    alignmentOffset100ns_=1250000; seekBoth(600000000,true);
    if(!wait([&] { return !left_->player->IsSeeking() && !right_->player->IsSeeking(); })) return fail("seek to middle");
    playBoth(); if(!wait([&] { return !syncing_ && left_->player->IsPlaying() && right_->player->IsPlaying(); })) return fail("linked play");
    QElapsedTimer elapsed; elapsed.start(); int64_t maxDrift=0,maxAudioLead=0; unsigned observations=0; int nextLog=10;
    while(elapsed.elapsed()<int64_t(durationSeconds)*1000) {
        QApplication::processEvents(); QThread::msleep(20);
        if(!left_->player->IsPlaying() || !right_->player->IsPlaying()) return fail("playback stopped before limit");
        if(elapsed.elapsed()<1000) continue;
        maxDrift=std::max(maxDrift,std::abs(right_->player->GetPosition100ns()-left_->player->GetPosition100ns()-alignmentOffset100ns_));
        for(auto panel:{left_,right_}) {
            if(panel->player->GetAudioError()!=Platform::kAudioErrorNone || panel->player->GetLastSubmittedAudioTime100ns()<0) return fail("real PCM output");
            maxAudioLead=std::max(maxAudioLead,panel->player->GetLastSubmittedAudioTime100ns()-panel->player->GetPosition100ns());
        }
        ++observations;
        if(elapsed.elapsed()>=nextLog*1000) { qInfo()<<"Playback seconds"<<nextLog<<"max frame drift ms"<<maxDrift/10000.0; nextLog+=10; }
    }
    left_->player->Pause(); right_->player->Pause();
    qInfo()<<"Playback duration seconds"<<durationSeconds<<"observations"<<observations<<"max frame drift ms"<<maxDrift/10000.0<<"max submitted audio lead ms"<<maxAudioLead/10000.0;
    if(maxDrift>1500000 || maxAudioLead>2500000 || observations<1000) return fail("CFR/VFR pacing exceeded bounds");
    if(!screenshotPath.isEmpty() && !grab().save(screenshotPath)) return fail("screenshot");
    left_->player->Close(); right_->player->Close();

    left_->player->SetMuted(false); right_->player->SetMuted(false);
    left_->setFile(nullptr,-1); right_->setFile(nullptr,-1); openFiles({mediaPath,QFileInfo(mediaPath).dir().filePath("vfr.mp4")}); if(!wait([&] { return left_->player->IsLoaded() && right_->player->IsLoaded(); })) return fail("reload CFR/VFR for negative offset"); left_->player->SetMuted(false); right_->player->SetMuted(false); alignmentOffset100ns_=-2500000; seekBoth(900000000,true);
    if(!wait([&] { return !left_->player->IsSeeking() && !right_->player->IsSeeking(); })) return fail("negative offset seek");
    playBoth(); if(!wait([&] { return !syncing_ && left_->player->IsPlaying() && right_->player->IsPlaying(); })) return fail("negative offset play");
    QElapsedTimer negTimer; negTimer.start(); int64_t negMaxDrift=0;
    while(negTimer.elapsed()<8000) {
        QApplication::processEvents(); QThread::msleep(20);
        if(!left_->player->IsPlaying() || !right_->player->IsPlaying()) return fail("negative offset stopped early");
        if(negTimer.elapsed()>=1000) negMaxDrift=std::max(negMaxDrift,std::abs(right_->player->GetPosition100ns()-left_->player->GetPosition100ns()-alignmentOffset100ns_));
    }
    left_->player->Pause(); right_->player->Pause();
    if(negMaxDrift>1500000) return fail("negative offset drift exceeded bounds");
    const auto fps24=QFileInfo(mediaPath).dir().filePath("fps24.mp4"),fps60=QFileInfo(mediaPath).dir().filePath("fps60.mp4");
    if(QFileInfo::exists(fps24) && QFileInfo::exists(fps60)) {
        openFiles({fps24,fps60});
        if(!wait([&] { return left_->player->IsLoaded() && right_->player->IsLoaded(); })) return fail("load 24fps/60fps pair");
        left_->player->SetMuted(false); right_->player->SetMuted(false);
        alignmentOffset100ns_=3500000; seekBoth(20000000,true);
        if(!wait([&] { return !left_->player->IsSeeking() && !right_->player->IsSeeking(); })) return fail("24/60 seek");
        playBoth(); if(!wait([&] { return !syncing_ && left_->player->IsPlaying() && right_->player->IsPlaying(); })) return fail("24/60 linked play");
        QElapsedTimer fpsTimer; fpsTimer.start(); int64_t fpsDrift=0;
        while(fpsTimer.elapsed()<6000) {
            QApplication::processEvents(); QThread::msleep(20);
            if(!left_->player->IsPlaying() || !right_->player->IsPlaying()) return fail("24/60 stopped early");
            if(fpsTimer.elapsed()>=1000) fpsDrift=std::max(fpsDrift,std::abs(right_->player->GetPosition100ns()-left_->player->GetPosition100ns()-alignmentOffset100ns_));
        }
        left_->player->Pause(); right_->player->Pause();
        if(fpsDrift>1500000) return fail("24/60 frame drift exceeded bounds");
        const auto [beginRange,endRange]=comparisonRange();
        seekBoth(endRange-12000000,true);
        if(!wait([&] { return !left_->player->IsSeeking() && !right_->player->IsSeeking(); })) return fail("24/60 near-EOF seek");
        playBoth(); if(!wait([&] { return !syncing_ && left_->player->IsPlaying() && right_->player->IsPlaying(); })) return fail("24/60 near-EOF play");
        if(!wait([&] { return !left_->player->IsPlaying() && !right_->player->IsPlaying(); })) return fail("24/60 overlap EOF stop");
        playBoth(right_);
        if(!wait([&] { return !syncing_ && left_->player->IsPlaying() && right_->player->IsPlaying() && left_->player->GetPosition100ns()<beginRange+15000000; })) return fail("24/60 EOF restart from begin");
        left_->player->Pause(); right_->player->Pause();
    }
    left_->player->Close(); right_->player->Close();
    qInfo()<<"PASS playback: CFR/VFR seconds="<<durationSeconds<<"(+125ms) + 8s (-250ms) + optional 24fps/60fps; maxDriftMs="<<maxDrift/10000.0<<"negDriftMs="<<negMaxDrift/10000.0<<"maxAudioLeadMs="<<maxAudioLead/10000.0<<"(software submission lead; physical DAC latency requires external loopback)";
    return true;
}

bool MainWindow::runOpenPairCheck(const QStringList& paths) {
    if(paths.size()!=2) return false;
    openFiles(paths);
    QElapsedTimer timer; timer.start();
    while((!left_->canvas->frame() || !right_->canvas->frame()) && timer.elapsed()<20000) { QApplication::processEvents(); QThread::msleep(2); }
    if(files_.size()!=2 || groups_.size()!=1 || !left_->canvas->frame() || !right_->canvas->frame()) return false;
    if(!options_.images) return true;
    auto canvas=right_->canvas; canvas->setTransform(1,{}); canvas->setComparison(left_->canvas,0);
    auto center=[](VideoCanvas* c) { return c->grab().toImage().pixelColor(c->rect().center()); };
    const auto own=center(canvas),other=center(left_->canvas);
    QKeyEvent down(QEvent::KeyPress,Qt::Key_B,Qt::NoModifier),up(QEvent::KeyRelease,Qt::Key_B,Qt::NoModifier);
    QApplication::sendEvent(canvas,&down);
    if(center(canvas)!=other) return false;
    QApplication::sendEvent(canvas,&up);
    if(center(canvas)!=own) return false;
    QApplication::sendEvent(canvas,&down); QFocusEvent focusOut(QEvent::FocusOut); QApplication::sendEvent(canvas,&focusOut);
    if(center(canvas)!=own) return false;
    canvas->setComparison(left_->canvas,2); QApplication::sendEvent(canvas,&down);
    if(center(canvas)!=other || canvas->comparisonMode()!=2) return false;
    QApplication::sendEvent(canvas,&up); canvas->setComparison(left_->canvas,0);
    left_->canvas->setPreviewHidden(true); QApplication::sendEvent(canvas,&down);
    if(center(canvas)!=own) return false;
    QApplication::sendEvent(canvas,&up); left_->canvas->setPreviewHidden(false);
    qInfo()<<"PASS command-line pair + A/B actual canvas: press/release/focus loss/view restoration/privacy; generated fixtures";
    return true;
}

bool MainWindow::runTransportCheck(const QString& mediaPath) {
    auto fail=[](const char* message) { qCritical("FAIL transport: %s",message); return false; };
    QTemporaryDir fixture; if(!fixture.isValid()) return fail("private fixture root");
    const auto a=fixture.filePath("a.mov"),b=fixture.filePath("b.mov");
    if(!QFile::copy(mediaPath,a) || !QFile::copy(mediaPath,b)) return fail("private copies");
    openFiles({a,b});
    auto wait=[&](const auto& condition) { QElapsedTimer clock; clock.start(); while(!condition() && clock.elapsed()<20000) { QApplication::processEvents(QEventLoop::AllEvents,20); QThread::msleep(2); } return condition(); };
    if(!wait([&] { return left_->player->IsLoaded() && right_->player->IsLoaded(); })) return fail("actual paired players");
    auto slider=left_->findChild<QSlider*>("playerSeek"); auto label=left_->findChild<QLabel*>("timeLabel");
    auto ready=[&] { return !left_->player->IsSeeking() && !right_->player->IsSeeking(); };
    for(const auto size:{QSize(1040,720),QSize(1360,920)}) {
        resize(size); QApplication::processEvents();
        if(slider->height()<28 || label->fontInfo().pixelSize()<13 || label->width()>label->sizeHint().width()+3 || slider->width()<100 || slider->geometry().right()>=label->geometry().left()) return fail("compact transport geometry/font");
    }
    slider->setValue(5000); if(!wait(ready)) return fail("initial seek");
    QStyleOptionSlider option; option.initFrom(slider); option.orientation=Qt::Horizontal; option.minimum=slider->minimum(); option.maximum=slider->maximum(); option.sliderPosition=slider->value(); option.sliderValue=slider->value();
    const auto handle=slider->style()->subControlRect(QStyle::CC_Slider,&option,QStyle::SC_SliderHandle,slider);
    const auto grab=QPointF(handle.center().x()+12,slider->height()/2.0);
    const auto old=slider->value(); int moved=0,released=0;
    auto moveConnection=connect(slider,&QSlider::sliderMoved,this,[&] { ++moved; });
    auto releaseConnection=connect(slider,&QSlider::sliderReleased,this,[&] { ++released; });
    auto mouse=[&](QEvent::Type type,QPointF point,Qt::MouseButton button,Qt::MouseButtons buttons) {
        QMouseEvent event(type,point,slider->mapToGlobal(point.toPoint()),button,buttons,Qt::NoModifier); QApplication::sendEvent(slider,&event);
    };
    mouse(QEvent::MouseButtonPress,grab,Qt::LeftButton,Qt::LeftButton);
    if(!slider->isSliderDown() || std::abs(slider->value()-old)>100) return fail("expanded hit area jumped");
    const auto destination=QPointF(slider->width()*0.75,slider->height()/2.0);
    mouse(QEvent::MouseMove,destination,Qt::NoButton,Qt::LeftButton);
    const auto target=left_->player->GetDuration100ns()*slider->value()/10000;
    mouse(QEvent::MouseButtonRelease,destination,Qt::LeftButton,Qt::NoButton);
    disconnect(moveConnection); disconnect(releaseConnection);
    if(!moved || released!=1 || slider->isSliderDown() || !wait(ready) || std::abs(left_->player->GetPosition100ns()-target)>1000000 || std::abs(left_->player->GetPosition100ns()-right_->player->GetPosition100ns())>1000000) return fail("actual mouse drag/live/exact linked seek");
    left_->setFile(nullptr,-1); right_->setFile(nullptr,-1); previewPool_.waitForDone(); left_->player->Close(); right_->player->Close();
    qInfo("PASS transport: 13px time label without fixed empty width, 28px hit area, 12px outside-thumb press, mouse drag/release reaches actual linked players; 1040/1360 layouts; no screenshot");
    return true;
}

bool MainWindow::runMatchingCheck(const QString& mediaPath,const QString& screenshotPath) {
    auto wait=[](const std::function<bool()>& ready) { QElapsedTimer time; time.start(); while(!ready() && time.elapsed()<15000) { QApplication::processEvents(); QThread::msleep(10); } return ready(); };
    auto fail=[](const char* why) { qWarning()<<"Matching check failed:"<<why; return false; };
    Language::setCurrent("en"); translate();
    QDir().mkpath(QFileInfo(screenshotPath).absolutePath()); QTemporaryDir fixture(QFileInfo(screenshotPath).absolutePath()+"/matching-files-XXXXXX"); if(!fixture.isValid()) return fail("fixture");
    QDir().mkpath(fixture.filePath("Archive")); QDir().mkpath(fixture.filePath("Incoming"));
    auto encode=QFileInfo(mediaPath).dir().filePath("encode.mp4"),other=QFileInfo(mediaPath).dir().filePath("different.mp4");
    for(const auto& name:{"A1.mp4","A2.mp4","C1.mp4","C2.mp4"}) if(!QFile::copy(QString(name).startsWith('A')?mediaPath:other,fixture.filePath("Archive/"+QString(name)))) return fail("archive copy");
    for(const auto& name:{"B1.mp4","B2.mp4"}) if(!QFile::copy(encode,fixture.filePath("Incoming/"+QString(name)))) return fail("incoming copy");
    folders_={QDir::toNativeSeparators(fixture.filePath("Archive")).toStdWString(),QDir::toNativeSeparators(fixture.filePath("Incoming")).toStdWString()};
    options_.minFileSize=1; options_.protectedFolders.clear(); options_.excludedFolders.clear(); options_.crossFolderOnly=true; options_.matchTransforms=true;
    profile_->setCurrentIndex(1); sort_->setCurrentIndex(int(SortOrder::NameAsc)); startScan();
    auto ready=[&] { return !scanning_ && pendingPreviews_.empty() && left_->player->IsLoaded() && right_->player->IsLoaded(); };
    if(!wait(ready) || groups_.size()!=2) {
        qWarning()<<"cross-folder state: groups"<<groups_.size()<<"scanning"<<scanning_<<"sortingMetadata"<<sortingMetadata_<<"pendingPreviews"<<pendingPreviews_.size()
                  <<"left"<<left_->fileIndex<<left_->player->IsLoaded()<<"right"<<right_->fileIndex<<right_->player->IsLoaded();
        return fail("cross-folder real scan");
    }
    auto validate=[&] {
        QSet<int> seen;
        for(const auto& group:groups_) {
            if(group.fileIndices.size()<2) return false;
            const auto& first=files_[group.fileIndices[0]];
            for(int member:group.fileIndices) { seen.insert(member); if(files_[member].fileName.starts_with(L"C")) return false; }
            for(size_t slot=1;slot<group.fileIndices.size();++slot) if(files_[group.fileIndices[slot]].comparisonRoot==first.comparisonRoot) return false;
        }
        return seen.size()==4;
    };
    if(!validate() || resultSummary_->text()!="2 groups · 4 files") return fail("cross-folder coverage/count/excludes within-root matches");
    sort_->setCurrentIndex(int(SortOrder::SizeAsc));
    if(!validate()) {
        for(const auto& group:groups_) { QStringList names; for(int member:group.fileIndices) names<<QString::fromStdWString(files_[member].fileName)+"@root"+QString::number(files_[member].comparisonRoot); qWarning()<<"group"<<names<<"manualReference"<<group.manualReference; }
        qWarning()<<"sortingMetadata"<<sortingMetadata_<<"reference rule"<<referenceRule_->currentText();
        return fail("cross-folder regroup after sort");
    }
    {
        // A file sharing its folder with another member cannot anchor a cross-folder group, even by hand.
        const auto before=groups_[0].fileIndices; int sameRoot=-1;
        for(int slot=1;slot<int(before.size());++slot) if(!canBeReference(groups_[0],before[slot])) { sameRoot=slot; break; }
        if(sameRoot<0) return fail("cross-folder fixture has a same-folder candidate");
        makeReference(0,sameRoot);
        if(groups_[0].fileIndices!=before || groups_[0].manualReference || !validate()) return fail("cross-folder make reference refused for a same-folder file");
    }
    if(!wait(ready)) return fail("sorted players");
    auto session=fixture.filePath("review.cvsession"); if(!saveSessionFile(session)) return fail("session save");
    options_.crossFolderOnly=false; options_.matchTransforms=false;
    // Current sessions reopen their saved results and signatures directly, without a rescan through the media cache.
    if(const bool loaded=loadSessionFile(session); !loaded || !wait(ready) || !options_.crossFolderOnly || !options_.matchTransforms || !validate() || !savedResultEvidence_) {
        qWarning()<<"round trip: loaded"<<loaded<<"ready"<<ready()<<"crossFolder"<<options_.crossFolderOnly<<"transforms"<<options_.matchTransforms<<"valid"<<validate()<<"savedResults"<<savedResultEvidence_;
        return fail("session/context/cache normalized signatures round trip");
    }
    QFile sessionInput(session); if(!sessionInput.open(QIODevice::ReadOnly)) return fail("session fixture read");
    auto savedSession=QJsonDocument::fromJson(sessionInput.readAll()).object(); sessionInput.close();
    auto writeSession=[&](const QJsonObject& object) {
        auto path=fixture.filePath("edited.cvsession"); QFile out(path); auto bytes=QJsonDocument(object).toJson();
        if(!out.open(QIODevice::WriteOnly|QIODevice::Truncate) || out.write(bytes)!=bytes.size()) return QString();
        return path;
    };
    auto rejectSession=[&](const std::function<void(QJsonObject&)>& edit) {
        auto object=savedSession; edit(object); auto path=writeSession(object); auto generation=generation_;
        return !path.isEmpty() && !loadSessionFile(path) && generation_==generation && !scanning_ && validate();
    };
    if(!rejectSession([](auto& o) { o["files"]="invalid"; }) ||
       !rejectSession([](auto& o) { o.remove("files"); }) ||
       !rejectSession([](auto& o) { o["version"]=999; }) ||
       !rejectSession([](auto& o) { auto a=o["files"].toArray(); a.append(a[0]); o["files"]=a; }) ||
       !rejectSession([](auto& o) { auto a=o["files"].toArray(); auto row=a[0].toObject(); row["path"]="relative.mp4"; a[0]=row; o["files"]=a; }) ||
       !rejectSession([](auto& o) { auto a=o["files"].toArray(); auto row=a[1].toObject(); row["identity"]=a[0].toObject()["identity"]; a[1]=row; o["files"]=a; }) ||
       !rejectSession([](auto& o) { auto c=o["criteria"].toObject(); c["threshold"]=101; o["criteria"]=c; }) ||
       !rejectSession([](auto& o) { auto c=o["criteria"].toObject(); c["tolerance"]="-1"; o["criteria"]=c; }) ||
       !rejectSession([](auto& o) { auto v=o["view"].toObject(); v["offset"]=3601; o["view"]=v; }) ||
       !rejectSession([](auto& o) { o["ignoredPairs"]=QJsonArray{"unknown/identity"}; })) return fail("invalid session must preserve current review");
    {
        auto huge=fixture.filePath("oversized.cvsession"); QFile out(huge);
        if(!out.open(QIODevice::WriteOnly) || !out.resize(33LL*1024*1024)) return fail("oversized fixture"); out.close();
        if(loadSessionFile(huge)) return fail("oversized session accepted");
    }
    auto protectedSession=savedSession; protectedSession["protectedFolders"]=QJsonArray{fixture.filePath("Archive")};
    auto unprotectedRows=protectedSession["files"].toArray();
    for(qsizetype i=0;i<unprotectedRows.size();++i) { auto row=unprotectedRows[i].toObject(); row["protected"]=false; row["folderProtected"]=false; unprotectedRows[i]=row; }
    protectedSession["files"]=unprotectedRows;
    if(!loadSessionFile(writeSession(protectedSession)) || !wait(ready)) return fail("edited folder protection load");
    for(const auto& file:files_) if(VideoScanner::IsWithinFolder(file.fullPath,folders_[0]) && (!file.protectedFile || !file.protectedByFolder)) return fail("session bypassed folder protection");
    // Missing/changed result members are reported and excluded, never silently replaced by new content.
    // (Only saved result members are checked; files outside the results are not part of a session.)
    { QFile changed(fixture.filePath("Incoming/B2.mp4")); if(!changed.open(QIODevice::Append) || changed.write("x")!=1) return fail("changed session fixture"); }
    if(!QFile::rename(fixture.filePath("Archive/A2.mp4"),fixture.filePath("Archive/A2.unavailable")) || !loadSessionFile(session) || !wait(ready)) return fail("load session with missing/changed members");
    {
        QStringList issues; for(const auto& issue:scanIssues_) issues<<QFileInfo(QString::fromStdWString(issue.path)).fileName();
        bool excluded=true; for(const auto& group:groups_) for(int member:group.fileIndices) if(files_[member].fileName==L"A2.mp4" || files_[member].fileName==L"B2.mp4") excluded=false;
        if(issues.size()!=2 || !issues.contains("A2.mp4") || !issues.contains("B2.mp4") || !excluded) { qWarning()<<"session members: issues"<<issues<<"excluded"<<excluded; return fail("missing/changed session members"); }
    }
    // Restore the missing file and rescan, so the remaining steps work on complete cross-folder groups again.
    if(!QFile::rename(fixture.filePath("Archive/A2.unavailable"),fixture.filePath("Archive/A2.mp4"))) return fail("restore missing member");
    startScan(); if(!wait(ready) || groups_.size()!=2 || !validate()) return fail("rescan after missing/changed members");
    bool detailsShown=false;
    QTimer::singleShot(0,this,[&] {
        if(auto dialog=findChild<QDialog*>("fileDetailsDialog")) { auto fps=dialog->findChild<QLabel*>("detailFps"); detailsShown=fps && fps->text().contains("12.000") && fps->width()>=300 && dialog->width()>=400; dialog->grab().save(screenshotPath+".details.png"); dialog->accept(); }
    });
    left_->findChild<QToolButton*>("fileDetails")->click(); if(!detailsShown) return fail("actual file details");
    auto pendingFile=files_[left_->fileIndex];
    { ScanCache journal(cachePath_); if(journal.beginRemoval(pendingFile)<=0) return fail("persist intent"); }
    { ScanCache reopened(cachePath_); if(!reopened.operationHistory().join(' ').contains("Outcome unconfirmed")) return fail("interrupted intent survives reopen"); }
    auto plan=cleanupPlan(); auto planPath=fixture.filePath("plan.csv");
    if(plan.empty() || !exportCleanupPlan(planPath,plan)) return fail("plan export");
    QFile planCsv(planPath); if(!planCsv.open(QIODevice::ReadOnly)) return fail("read plan export");
    auto planBytes=planCsv.readAll();
    if(!planBytes.startsWith(QByteArray::fromHex("efbbbf")) || planBytes.count("planned_recycle_bin")!=int(plan.size()) || !QFileInfo::exists(QString::fromStdWString(plan.front().file.fullPath))) return fail("plan export content/source unchanged");
    {
        ScanCache journal(cachePath_); auto diagnostic=pendingFile; diagnostic.fullPath=L"=formula,\"Türkçe\".mp4";
        journal.recordRemoval(diagnostic,E_ACCESSDENIED);
        if(!journal.exportOperationHistory(fixture.filePath("history.csv"))) return fail("history export");
        QFile csv(fixture.filePath("history.csv")); if(!csv.open(QIODevice::ReadOnly)) return fail("read history export");
        auto bytes=csv.readAll();
        if(!bytes.contains("unconfirmed") || !bytes.contains("failed") || !bytes.contains("0x80070005") || !bytes.contains(Csv::field(QString::fromStdWString(diagnostic.fullPath)).toUtf8()) || Csv::field("=1")!="\"'=1\"") return fail("history export outcomes/CSV escaping");
    }
    bool privacyShown=false;
    QTimer::singleShot(0,this,[&] { if(auto box=qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) { privacyShown=box->text().contains("thumbnails") && box->text().contains("operation history"); box->accept(); } });
    findChild<QAction*>("Data and privacy")->trigger(); if(!privacyShown) return fail("actual data policy");
    bool decoderReport=false; auto originalIssues=scanIssues_; scanIssues_.push_back({pendingFile.fullPath,Platform::kErrorCodecNotFound});
    QTimer::singleShot(0,this,[&] { if(auto dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget())) { auto report=dialog->findChild<QPlainTextEdit*>(); decoderReport=report && report->toPlainText().contains(MainWindowDetail::readError(Platform::kErrorCodecNotFound)); dialog->accept(); } });
    showScanReport(); scanIssues_=std::move(originalIssues); if(!decoderReport) return fail("missing decoder report explanation");
    // macOS shows About as a modeless window, so it is looked up among the top-level windows too.
    bool aboutShown=false,aboutLooked=false;
    QTimer::singleShot(100,this,[&] {
        auto box=qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        for(auto widget:QApplication::topLevelWidgets()) if(!box && widget->isVisible()) box=qobject_cast<QMessageBox*>(widget);
        aboutLooked=true;
        if(box) { aboutShown=box->text().contains(QCoreApplication::applicationVersion()) && windowTitle().contains(QCoreApplication::applicationVersion()); box->accept(); }
    });
    findChild<QAction*>("About Compareel")->trigger(); if(!wait([&] { return aboutLooked; }) || !aboutShown) return fail("actual package version in About/window title");
    if(!screenshotPath.isEmpty() && !grab().save(screenshotPath)) return fail("screenshot");
    auto usableCache=cachePath_; auto guardedPath=QString::fromStdWString(files_[right_->fileIndex].fullPath);
    cachePath_=fixture.filePath("missing-parent/state.sqlite");
    QTimer dismissError; connect(&dismissError,&QTimer::timeout,this,[] { if(auto box=qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) box->accept(); }); dismissError.start(20);
    right_->findChild<QToolButton*>("trashRight")->click();
    if(!wait([&] { return !fileOperationPending_ && !QApplication::activeModalWidget(); }) || !QFileInfo::exists(guardedPath)) return fail("journal failure must not remove file");
    dismissError.stop(); cachePath_=usableCache; if(!wait(ready)) return fail("guarded player reload");
    int removedIndex=right_->fileIndex; auto removedPath=QString::fromStdWString(files_[removedIndex].fullPath);
    right_->findChild<QToolButton*>("trashRight")->click();
    if(!wait([&] { return !fileOperationPending_; }) || QFileInfo::exists(removedPath)) return fail("shared cross-folder candidate trash");
    for(const auto& group:groups_) for(int member:group.fileIndices) if(member==removedIndex) return fail("deleted shared candidate remains in another group");
    // A warm-cache exact pass must also obey cross-folder scope; no same-root-only group.
    profile_->setCurrentIndex(0); startScan(); if(!wait([&] { return !scanning_; }) || !groups_.empty()) return fail("exact mode cross-folder scope");
    left_->setFile(nullptr,-1); right_->setFile(nullptr,-1); left_->player->Close(); right_->player->Close(); previewPool_.waitForDone();

    qInfo()<<"PASS matching: actual cross-folder visual scan/all four matched files/same-root-only exclusion, sort regroup, normalized cache/session flags, real details/data policy, plan/history CSV with pending/error/escaping, exact scope";
    return true;
}

bool MainWindow::runReviewToolsCheck(const QString& mediaPath,const QString& screenshotPath) {
    (void)mediaPath;
    auto wait=[](const std::function<bool()>& ready,int timeoutMs=15000) {
        QElapsedTimer clock; clock.start();
        while(!ready() && clock.elapsed()<timeoutMs) { QApplication::processEvents(QEventLoop::AllEvents,20); QThread::msleep(5); }
        return ready();
    };
    auto fail=[](const char* reason) { qCritical("FAIL review tools: %s",reason); return false; };

    // 1. Accessibility (accessibleName, toolTip, tab chain, keyboard shortcuts) & Scaling (1040x720 + simulated 150%/200% DPI)
    for(bool tr:{false,true}) {
        Language::setCurrent(tr?"tr":"en"); translate(); QApplication::processEvents();
        const QStringList requiredToolButtons{"sourcesButton","primaryButton","privacyButton","stopButton","linkControls","autoAlignPartialClip","resetAlignment","previousGroupButton","nextGroupButton"};
        for(const auto& name:requiredToolButtons) {
            auto btn=findChild<QToolButton*>(name);
            if(!btn || btn->accessibleName().trimmed().isEmpty() || btn->toolTip().trimmed().isEmpty()) {
                qCritical()<<"Missing accessibleName/toolTip on toolButton"<<name<<"tr="<<tr;
                return fail("main toolbar/bridge button accessibility");
            }
        }
        for(auto panel:{left_,right_}) {
            if(panel->canvas->accessibleName().trimmed().isEmpty()) return fail("canvas accessibleName");
            for(const auto& editName:{"filename","folderPath"}) {
                auto edit=panel->findChild<QLineEdit*>(editName);
                if(!edit || edit->accessibleName().trimmed().isEmpty()) return fail("player panel LineEdit accessibleName");
            }
            auto seek=panel->findChild<QSlider*>("playerSeek");
            if(!seek || seek->accessibleName().trimmed().isEmpty() || seek->toolTip().trimmed().isEmpty()) return fail("playerSeek accessibleName/toolTip");
            for(auto btn:panel->findChildren<QToolButton*>()) {
                if(btn->accessibleName().trimmed().isEmpty() || btn->toolTip().trimmed().isEmpty()) return fail("player panel ToolButton accessibleName/toolTip");
            }
        }
        if(mediaMode_->accessibleName().isEmpty() || mediaMode_->toolTip().isEmpty() ||
           profile_->accessibleName().isEmpty() || profile_->toolTip().isEmpty() ||
           offset_->accessibleName().isEmpty() || offset_->toolTip().isEmpty() ||
           filter_->accessibleName().isEmpty() || filter_->toolTip().isEmpty() ||
           sort_->accessibleName().isEmpty() || sort_->toolTip().isEmpty() ||
           results_->accessibleName().isEmpty()) return fail("combo/filter/spinbox/tree accessibleName or toolTip");
    }
    Language::setCurrent("en"); translate(); QApplication::processEvents();
    {
        QList<QWidget*> expectedChain{
            findChild<QToolButton*>("sourcesButton"),
            mediaMode_,
            profile_,
            findChild<QToolButton*>("primaryButton"),
            findChild<QToolButton*>("stopButton"),
            findChild<QToolButton*>("privacyButton"),
            left_->findChild<QLineEdit*>("filename"),
            left_->findChild<QToolButton*>("fileDetails"),
            findChild<QToolButton*>("linkControls"),
            right_->findChild<QLineEdit*>("filename"),
            right_->findChild<QToolButton*>("fileDetails"),
            findChild<QToolButton*>("autoAlignPartialClip"),
            offset_,
            findChild<QToolButton*>("resetAlignment"),
            findChild<QToolButton*>("previousGroupButton"),
            findChild<QToolButton*>("nextGroupButton"),
            filter_, evidenceFilter_,
            sort_,
            results_
        };
        for(auto* w:expectedChain) if(!w) return fail("expected tab chain widget missing");
        QWidget* cur=expectedChain.front();
        int matched=1;
        for(int step=0;step<128 && matched<expectedChain.size();++step) { // the review bar adds stops before the results
            cur=cur->nextInFocusChain();
            if(!cur) break;
            if(cur==expectedChain[matched]) ++matched;
        }
        if(matched!=expectedChain.size()) {
            qCritical()<<"Tab chain matched"<<matched<<"of"<<expectedChain.size();
            return fail("logical tab order chain");
        }
        bool shortcutsDialogOk=false;
        QTimer::singleShot(0,this,[&] {
            if(auto box=qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                const auto txt=box->text();
                shortcutsDialogOk=txt.contains("F5") && txt.contains("Space") && txt.contains(QKeySequence("Ctrl+L").toString(QKeySequence::NativeText)) && txt.contains(QKeySequence("Ctrl+Shift+A").toString(QKeySequence::NativeText)) && txt.contains("F1");
                box->accept();
            }
        });
        findChild<QAction*>("Keyboard shortcuts")->trigger();
        if(!shortcutsDialogOk) return fail("keyboard shortcuts dialog listing");
    }
    auto layoutIntact=[&] {
        // Before a pair is loaded the empty state replaces the players; it must fit the window.
        if(emptyState_->isVisible()) return rect().contains(QRect(emptyState_->mapTo(this,QPoint(0,0)),emptyState_->size())) && emptyState_->height()>=120;
        for(auto panel:{left_,right_}) {
            auto seek=panel->findChild<QSlider*>("playerSeek"); auto name=panel->findChild<QLineEdit*>("filename"); auto folder=panel->findChild<QLineEdit*>("folderPath");
            QRect canvas(panel->canvas->mapTo(panel,QPoint(0,0)),panel->canvas->size());
            QRect transport(seek->mapTo(panel,QPoint(0,0)),seek->size());
            QRect info(folder->mapTo(panel,QPoint(0,0)),folder->size());
            QRect heading(name->mapTo(panel,QPoint(0,0)),name->size());
            if(canvas.height()<48 || canvas.intersects(transport) || canvas.intersects(info) || heading.bottom()>=canvas.top() || transport.bottom()>=info.top()) return false;
        }
        auto align=findChild<QWidget*>("alignmentControls");
        return align && rect().contains(QRect(align->mapTo(this,QPoint(0,0)),align->size()));
    };
    const QFont baseFont=QApplication::font();
    const auto desktopMax=screen()?screen()->availableGeometry().size()-(frameGeometry().size()-size())-QSize(2,2):QSize(1360,920);
    for(int pt:{10,13,15}) {
        QApplication::setFont(QFont("Segoe UI",pt));
        for(const auto& req:{QSize(1040,720),QSize(1360,920)}) {
            resize(req.boundedTo(desktopMax)); QApplication::processEvents();
            if(!layoutIntact()) { qWarning()<<"layout at"<<pt<<"pt size"<<size()<<"requested"<<req<<"minimum hint"<<minimumSizeHint(); QApplication::setFont(baseFont); return fail("scaled/compact layout geometry overlap"); }
        }
    }
    QApplication::setFont(baseFont); resize(QSize(1360,920).boundedTo(desktopMax)); QApplication::processEvents();

    // 2. Scan report storage tier & disconnected/unsupported error explanations
    {
        auto prevMetrics=scanMetrics_; auto prevIssues=scanIssues_; auto prevManual=manual_;
        manual_=false;
        scanMetrics_.requestedWorkers=4; scanMetrics_.readWorkers=4; scanMetrics_.sampleWorkers=2;
        scanMetrics_.primaryStorageTier=static_cast<int>(StorageTier::Rotational);
        scanMetrics_.volumeCap=1;
        scanIssues_={{L"C:\\synthetic\\disconnected.mp4",HRESULT_FROM_WIN32(ERROR_DEV_NOT_EXIST)},{L"C:\\synthetic\\unsupported.mkv",Platform::kErrorUnsupportedByteStream}};
        bool reportOk=false;
        QTimer::singleShot(0,this,[&] {
            if(auto dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
                auto edit=dialog->findChild<QPlainTextEdit*>();
                const QString body=edit?edit->toPlainText():QString();
                reportOk=body.contains("Detected storage tier: HDD (rotational) · per-volume active limit: 1.") &&
                         body.contains("Storage device or network share disconnected or timed out") &&
                         body.contains("Unsupported media container or stream format");
                dialog->accept();
            }
        });
        showScanReport();
        scanMetrics_=prevMetrics; scanIssues_=prevIssues; manual_=prevManual;
        if(!reportOk) return fail("scan report storage tier and disconnected/unsupported error messages");
    }

    // 3. Automatic Partial Clip Relationship & Alignment UI
    QTemporaryDir fixture; if(!fixture.isValid()) return fail("temporary fixture dir");
    QString ffmpegExe="C:/ffmpeg/bin/ffmpeg.exe";
    if(!QFileInfo::exists(ffmpegExe)) ffmpegExe=QStandardPaths::findExecutable("ffmpeg");
    if(ffmpegExe.isEmpty()) return fail("ffmpeg executable for synthetic partial-clip fixtures");
    auto runFfmpeg=[&](const QStringList& args) {
        QProcess proc; proc.start(ffmpegExe,args);
        return proc.waitForFinished(30000) && proc.exitCode()==0;
    };
    const QString fullClip=fixture.filePath("full_6s.mp4");
    const QString trimStartClip=fixture.filePath("trim_start_4s.mp4");
    const QString sharedMidClip=fixture.filePath("shared_mid_6s.mp4");
    const QString unrelatedClip=fixture.filePath("unrelated_6s.mp4");
    const QString staticBlackA=fixture.filePath("static_black_a.mp4");
    const QString staticBlackB=fixture.filePath("static_black_b.mp4");
    if(!runFfmpeg({"-y","-hide_banner","-loglevel","error","-f","lavfi","-i","testsrc2=size=320x180:rate=24:duration=6.0","-c:v","libx264","-pix_fmt","yuv420p","-preset","ultrafast","-crf","18",fullClip}))
        return fail("generate full_6s.mp4");
    if(!runFfmpeg({"-y","-hide_banner","-loglevel","error","-ss","2.0","-i",fullClip,"-t","4.0","-c:v","libx264","-pix_fmt","yuv420p","-preset","ultrafast","-crf","18",trimStartClip}))
        return fail("generate trim_start_4s.mp4");
    if(!runFfmpeg({"-y","-hide_banner","-loglevel","error",
                   "-f","lavfi","-i","color=c=green:size=320x180:rate=24:duration=1.5",
                   "-ss","1.5","-t","3.0","-i",fullClip,
                   "-f","lavfi","-i","color=c=blue:size=320x180:rate=24:duration=1.5",
                   "-filter_complex","[0:v][1:v][2:v]concat=n=3:v=1:a=0[v]","-map","[v]",
                   "-c:v","libx264","-pix_fmt","yuv420p","-preset","ultrafast","-crf","18",sharedMidClip}))
        return fail("generate shared_mid_6s.mp4");
    if(!runFfmpeg({"-y","-hide_banner","-loglevel","error","-f","lavfi","-i","smptebars=size=320x180:rate=24:duration=6.0","-vf","negate,hflip","-c:v","libx264","-pix_fmt","yuv420p","-preset","ultrafast","-crf","18",unrelatedClip}))
        return fail("generate unrelated_6s.mp4");
    if(!runFfmpeg({"-y","-hide_banner","-loglevel","error","-f","lavfi","-i","color=c=black:size=320x180:rate=24:duration=5.0","-c:v","libx264","-pix_fmt","yuv420p","-preset","ultrafast",staticBlackA}) ||
       !runFfmpeg({"-y","-hide_banner","-loglevel","error","-f","lavfi","-i","color=c=black:size=320x180:rate=24:duration=5.0","-c:v","libx264","-pix_fmt","yuv420p","-preset","ultrafast",staticBlackB}))
        return fail("generate static_black fixtures");

    auto alignBtn=findChild<QToolButton*>("autoAlignPartialClip");
    if(!alignBtn) return fail("autoAlignPartialClip button");

    left_->setFile(nullptr,-1); right_->setFile(nullptr,-1);
    openFiles({trimStartClip,fullClip});
    if(!wait([&] { return left_->player->IsLoaded() && right_->player->IsLoaded() && alignBtn->isEnabled(); })) return fail("load trimStart/full pair");
    bool alignmentHeartbeat=false;
    QTimer::singleShot(0,this,[&] { alignmentHeartbeat=true; });
    QElapsedTimer alignmentReturn; alignmentReturn.start();
    alignBtn->click();
    // macOS slides the progress sheet in during show(), about 250 ms; the work itself runs on a worker thread.
    if(!sortingMetadata_ || alignmentReturn.elapsed()>500) return fail("alignment blocks GUI thread");
    if(!wait([&] { return !sortingMetadata_; })) return fail("asynchronous alignment completion");
    if(!alignmentHeartbeat) return fail("alignment GUI heartbeat");
    if(std::abs(offset_->value()-2.0)>0.6 || !similarity_->toolTip().contains("Partial clip match:") || !similarity_->toolTip().contains("not a full copy")) {
        qCritical()<<"Partial clip Case A failed: offset="<<offset_->value()<<"text="<<similarity_->text();
        return fail("partial clip start-trimmed alignment (+2.0s)");
    }

    left_->setFile(nullptr,-1); right_->setFile(nullptr,-1);
    openFiles({fullClip,sharedMidClip});
    if(!wait([&] { return left_->player->IsLoaded() && right_->player->IsLoaded() && alignBtn->isEnabled(); })) return fail("load full/sharedMid pair");
    alignBtn->click();
    if(!wait([&] { return !sortingMetadata_; })) return fail("asynchronous alignment completion");
    if(std::abs(offset_->value()-0.0)>0.6 || !similarity_->text().contains("Partial clip match:")) {
        qCritical()<<"Partial clip Case B failed: offset="<<offset_->value()<<"text="<<similarity_->text();
        return fail("partial clip shared middle segment alignment");
    }

    left_->setFile(nullptr,-1); right_->setFile(nullptr,-1);
    openFiles({fullClip,unrelatedClip});
    if(!wait([&] { return left_->player->IsLoaded() && right_->player->IsLoaded() && alignBtn->isEnabled(); })) return fail("load full/unrelated pair");
    alignBtn->click();
    if(!wait([&] { return !sortingMetadata_; })) return fail("asynchronous alignment completion");
    if(offset_->value()!=0.0 || !similarity_->text().contains("not found")) {
        qCritical()<<"Partial clip Case C failed: offset="<<offset_->value()<<"text="<<similarity_->text();
        return fail("partial clip unrelated pair rejection");
    }

    left_->setFile(nullptr,-1); right_->setFile(nullptr,-1);
    openFiles({staticBlackA,staticBlackB});
    if(!wait([&] { return left_->player->IsLoaded() && right_->player->IsLoaded() && alignBtn->isEnabled(); })) return fail("load static black pair");
    alignBtn->click();
    if(!wait([&] { return !sortingMetadata_; })) return fail("asynchronous alignment completion");
    if(similarity_->text().contains("Partial clip match:")) return fail("static black clips must not produce partial clip match");
    offset_->setValue(0.75);
    alignBtn->click();
    auto alignProgress=findChild<QProgressDialog*>("alignmentProgress");
    if(!alignProgress || !alignmentWatcher_) return fail("alignment cancel UI missing");
    auto cancelButton=alignProgress->findChild<QPushButton*>();
    if(!cancelButton) return fail("alignment cancel button missing");
    cancelButton->click();
    if(!wait([&] { return !sortingMetadata_; }) || !cancel_.load() || offset_->value()!=0.75) return fail("cancelled alignment applied stale offset");
    {
        MainWindow closeDuringAlignment(fixture.filePath("alignment-close-settings"));
        closeDuringAlignment.rememberReviewAction_->setChecked(false);
        closeDuringAlignment.openFiles({trimStartClip,fullClip});
        if(!wait([&] { return closeDuringAlignment.left_->player->IsLoaded() && closeDuringAlignment.right_->player->IsLoaded(); })) return fail("alignment close fixture load");
        closeDuringAlignment.autoAlignPartialClip(); closeDuringAlignment.close();
        if(!closeDuringAlignment.cancel_.load() || !wait([&] { return !closeDuringAlignment.alignmentWatcher_ && closeDuringAlignment.closeReady_; })) return fail("close during alignment hangs or leaves worker");
    }

    // 4. Advanced Bulk Retention Rules UI & Safety Guarantees
    const QString bulkRoot=fixture.filePath("bulk");
    const QString dirArchive=QDir(bulkRoot).filePath("Archive");
    const QString dirIncoming=QDir(bulkRoot).filePath("Incoming");
    if(!QDir().mkpath(dirArchive) || !QDir().mkpath(dirIncoming)) return fail("create bulk retention directories");

    // "_archive" rather than ".mp4" right after "320p": "_" and "." sort differently on Windows and in macOS/ICU collation.
    const QString g1LowResArchive=QDir(dirArchive).filePath("G1_320p_archive.mp4");
    const QString g1HighResIncoming=QDir(dirIncoming).filePath("G1_640p.mp4");
    const QString g1SmallIncoming=QDir(dirIncoming).filePath("G1_320p_small.mp4");
    if(!QFile::copy(fullClip,g1LowResArchive)) return fail("copy G1_320p");
    if(!runFfmpeg({"-y","-hide_banner","-loglevel","error","-i",fullClip,"-vf","scale=640:360","-c:v","libx264","-pix_fmt","yuv420p","-preset","ultrafast","-crf","16",g1HighResIncoming}))
        return fail("generate G1_640p");
    if(!runFfmpeg({"-y","-hide_banner","-loglevel","error","-i",fullClip,"-c:v","libx264","-pix_fmt","yuv420p","-preset","ultrafast","-crf","30",g1SmallIncoming}))
        return fail("generate G1_320p_small");

    left_->setFile(nullptr,-1); right_->setFile(nullptr,-1); left_->player->Close(); right_->player->Close();
    folders_={QDir::toNativeSeparators(dirArchive).toStdWString(),QDir::toNativeSeparators(dirIncoming).toStdWString()};
    options_.minFileSize=1; options_.protectedFolders.clear(); options_.excludedFolders.clear(); options_.crossFolderOnly=false;
    // The reference (slot 0) is what "Reference file" keeps; pin it to name order so the expected keeper is known.
    profile_->setCurrentIndex(1); sort_->setCurrentIndex(int(SortOrder::NameAsc)); referenceRule_->setCurrentIndex(referenceRule_->findData(int(ReferenceRule::NameOrder)));
    startScan();
    if(!wait([&] { return !scanning_ && !sortingMetadata_ && pendingPreviews_.empty() && left_->player->IsLoaded() && right_->player->IsLoaded(); },20000) || groups_.size()!=1 || groups_[0].fileIndices.size()!=3)
        return fail("bulk retention similar group scan");

    {
        CleanupRetentionRule rSort{RetentionAttribute::CurrentSortOrder,{}};
        auto planSort=cleanupPlan(rSort);
        if(planSort.size()!=2 || QFileInfo(QString::fromStdWString(planSort[0].keeper.fullPath)).fileName()!="G1_320p_archive.mp4")
            return fail("default CurrentSortOrder keeper (#1 NameAsc = G1_320p_archive.mp4)");

        CleanupRetentionRule rHiRes{RetentionAttribute::ResolutionHighest,{}};
        auto planHiRes=cleanupPlan(rHiRes);
        if(planHiRes.size()!=2 || QFileInfo(QString::fromStdWString(planHiRes[0].keeper.fullPath)).fileName()!="G1_640p.mp4")
            return fail("ResolutionHighest keeper (G1_640p.mp4)");

        CleanupRetentionRule rSmallest{RetentionAttribute::SizeSmallest,{}};
        auto planSmallest=cleanupPlan(rSmallest);
        if(planSmallest.size()!=2 || QFileInfo(QString::fromStdWString(planSmallest[0].keeper.fullPath)).fileName()!="G1_320p_small.mp4")
            return fail("SizeSmallest keeper (G1_320p_small.mp4)");

        CleanupRetentionRule rPrefFolder{RetentionAttribute::CurrentSortOrder,QDir::toNativeSeparators(dirIncoming).toStdWString()};
        auto planPref=cleanupPlan(rPrefFolder);
        if(planPref.size()!=2 || QFileInfo(QString::fromStdWString(planPref[0].keeper.fullPath)).fileName()!="G1_320p_small.mp4")
            return fail("PreferredFolder Incoming + SortOrder tie-breaker (G1_320p_small.mp4 before G1_640p.mp4 in NameAsc)");

        int archiveIdx=-1;
        for(int idx:groups_[0].fileIndices) if(files_[idx].fileName==L"G1_320p_archive.mp4") archiveIdx=idx;
        if(archiveIdx<0) return fail("locate G1_320p index");
        files_[archiveIdx].protectedFile=true;
        auto planProtected=cleanupPlan(rHiRes);
        if(planProtected.size()!=1 || QFileInfo(QString::fromStdWString(planProtected[0].file.fullPath)).fileName()!="G1_320p_small.mp4" ||
           QFileInfo(QString::fromStdWString(planProtected[0].keeper.fullPath)).fileName()!="G1_640p.mp4") {
            return fail("protected non-keeper must be excluded from cleanupPlan");
        }
        files_[archiveIdx].protectedFile=false;
    }

    bool dialogVerified=false;
    QTimer::singleShot(0,this,[&] {
        auto dialog=findChild<QDialog*>("bulkCleanupPlan");
        if(!dialog) return;
        auto ruleCombo=dialog->findChild<QComboBox*>("retentionRuleCombo");
        auto folderCombo=dialog->findChild<QComboBox*>("preferredFolderCombo");
        auto tieNote=dialog->findChild<QLabel*>("retentionTieBreakerNote");
        auto tree=dialog->findChild<QTreeWidget*>("cleanupPlanTree");
        if(ruleCombo && folderCombo && tieNote && tree && tieNote->text().contains("does not guarantee higher visual quality")) {
            ruleCombo->setCurrentIndex(1);
            QApplication::processEvents();
            if(planRows(tree)==2 && tree->topLevelItem(0)->text(0).endsWith("G1_640p.mp4")) {
                dialogVerified=true;
                if(!screenshotPath.isEmpty()) dialog->grab().save(screenshotPath);
                dialog->accept();
                return;
            }
        }
        dialog->reject();
    });
    previewBulkCleanup();
    if(!dialogVerified || !wait([&] { return !fileOperationPending_; })) return fail("previewBulkCleanup interactive rule selection & execution");
    if(!QFileInfo::exists(g1HighResIncoming) || QFileInfo::exists(g1LowResArchive) || QFileInfo::exists(g1SmallIncoming))
        return fail("bulk cleanup with Highest resolution rule must keep only G1_640p.mp4");

    left_->setFile(nullptr,-1); right_->setFile(nullptr,-1); left_->player->Close(); right_->player->Close(); previewPool_.waitForDone();
    {
        // 20,000-file (10,000-group) O(N log N) cleanupPlan benchmark
        files_.assign(20000,{});
        groups_.assign(10000,{});
        for(size_t i=0;i<files_.size();++i) {
            files_[i].fullPath=(i%2==0?L"C:\\Archive\\v_":L"C:\\Incoming\\v_")+std::to_wstring(i)+L".mp4";
            files_[i].folderPath=i%2==0?L"C:\\Archive":L"C:\\Incoming";
            files_[i].fileName=L"v_"+std::to_wstring(i)+L".mp4";
            files_[i].fileSize=10000+i/2;
            files_[i].width=i%2==0?1280:1920;
            files_[i].height=i%2==0?720:1080;
            files_[i].metadataLoaded=true;
            files_[i].identity={1,1000+i,files_[i].fileSize,1000+i,true};
            // The plan re-checks each target against its keeper, so pairs need real matching evidence.
            files_[i].contentHash=std::to_string(i/2);
        }
        for(size_t g=0;g<groups_.size();++g) groups_[g].fileIndices={int(g*2),int(g*2+1)};
        const auto savedCriteria=scanCriteria_; const bool savedManual=manual_;
        scanCriteria_=ScanCriteria{}; scanCriteria_.matchContent=true; manual_=false;
        QElapsedTimer planClock; planClock.start();
        const auto largePlan=cleanupPlan({RetentionAttribute::ResolutionHighest,L"C:\\Incoming"});
        const auto planMs=planClock.elapsed();
        scanCriteria_=savedCriteria; manual_=savedManual;
        files_.clear(); groups_.clear();
        if(largePlan.size()!=10000 || planMs>500) return fail("20k-file O(N log N) cleanupPlan scale");
        qInfo()<<"20,000-file (10,000-group) cleanupPlan elapsed_ms="<<planMs<<"targets="<<largePlan.size();
    }
    {
        // Ctrl+I regroups in memory: cleanup marks and unrelated groups survive; marked removal keeps an unmarked match.
        files_.assign(5,{});
        for(size_t i=0;i<files_.size();++i) {
            files_[i].fullPath=L"C:\\Marks\\m_"+std::to_wstring(i)+L".mp4"; files_[i].folderPath=L"C:\\Marks"; files_[i].fileName=L"m_"+std::to_wstring(i)+L".mp4";
            files_[i].fileSize=100; files_[i].metadataLoaded=true; files_[i].contentHash=i<3?"a":"b"; files_[i].identity={1,5000+i,100,1,true};
        }
        const auto savedCriteria=scanCriteria_; const bool savedManual=manual_;
        scanCriteria_=ScanCriteria{}; scanCriteria_.matchContent=true; manual_=false;
        groups_.assign(2,{}); groups_[0].fileIndices={0,1,2}; groups_[0].rightSlot=1; groups_[1].fileIndices={3,4}; groups_[1].rightSlot=1;
        populateResults(); markedFiles_={1,2,4};
        {
            // Each thumbnail shows its own cleanup mark, protection and pair decision.
            files_[3].protectedFile=true; reviewDecisions_[ReviewPairKey(files_[0].identity,files_[2].identity)]=ReviewDecision::Later;
            updateGroupRow(0); updateGroupRow(1);
            auto badges=[this](int fileIndex) {
                QStringList tips;
                for(int g=0;g<int(groups_.size());++g) for(int slot=0;slot<int(groups_[g].fileIndices.size());++slot)
                    if(groups_[g].fileIndices[slot]==fileIndex) tips<<thumbnailView(g,slot).badges;
                return tips;
            };
            if(badges(0)!=QStringList{} || badges(1)!=QStringList{text("Marked for cleanup")} || badges(2)!=QStringList{text("Marked for cleanup"),text("Review later")} ||
               badges(3)!=QStringList{text("Protected")}) return fail("thumbnail state badges");
            if(const auto shots=qEnvironmentVariable("COMPAREEL_CHECK_SHOTS"); !shots.isEmpty()) { QApplication::processEvents(); results_->grab().save(shots+"/thumbnail-badges.png"); }
            files_[3].protectedFile=false; reviewDecisions_.clear(); markedFiles_={1,2,4}; updateGroupRow(0); updateGroupRow(1);
            qInfo()<<"Thumbnail badges show cleanup mark, protection and pair decision";
        }
        const auto plan=markedCleanupPlan();
        if(plan.size()!=3) return fail("marked cleanup plan");
        for(const auto& target:plan) if(target.keeper.identity.index!=5000 && target.keeper.identity.index!=5003) return fail("marked file chosen as keeper");
        currentGroup_=0; left_->fileIndex=0; right_->fileIndex=1;
        ignoreCurrentPair();
        const bool pairSplit=std::none_of(groups_.begin(),groups_.end(),[](const auto& group) {
            return std::find(group.fileIndices.begin(),group.fileIndices.end(),0)!=group.fileIndices.end() && std::find(group.fileIndices.begin(),group.fileIndices.end(),1)!=group.fileIndices.end(); });
        const bool otherKept=std::any_of(groups_.begin(),groups_.end(),[](const auto& group) { return group.fileIndices==std::vector<int>{3,4}; });
        if(scanning_ || !pairSplit || !otherKept || markedFiles_!=QSet<int>{1,2,4}) return fail("Ctrl+I in-memory regroup must keep marks and other groups");
        { ScanCache cache(cachePath_); cache.clearIgnoredPairs(); }
        left_->setFile(nullptr,-1); right_->setFile(nullptr,-1); markedFiles_.clear(); files_.clear(); groups_.clear(); currentGroup_=-1; populateResults();
        scanCriteria_=savedCriteria; manual_=savedManual;
        qInfo()<<"Ctrl+I in-memory regroup kept marks/other groups; marked removal keeps unmarked matches";
    }
    qInfo()<<"PASS review tools: EN/TR accessibility & tab chain, 1040x720 + simulated 150%/200% font scaling, scan report storage tier & error explanations, partial-clip alignment (+2.0s trim, shared mid, unrelated/static rejection), and bulk retention rules (sort/resolution/size/preferred folder/protections/execution)";
    return true;
}
