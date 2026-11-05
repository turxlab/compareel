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

bool MainWindow::runCriteriaCheck() {
    auto fail=[](int line) { qCritical("FAIL criteria: step at MainWindowChecksScan.cpp:%d",line); return false; };
    if(!similarityThreshold_) return fail(__LINE__);
    profile_->setCurrentIndex(0); QApplication::processEvents();
    if(!similarityThreshold_->isHidden()) return fail(__LINE__);
    profile_->setCurrentIndex(1); QApplication::processEvents();
    // On the welcome card the toolbar is hidden; the threshold is reached through the card's rules button instead.
    const bool welcome=!emptyState_->isHidden();
    if(similarityThreshold_->isHidden()!=welcome || !similarityThreshold_->toolTip().contains("false matches")) return fail(__LINE__);
    if(welcome && (!emptyState_->findChild<QToolButton*>("emptyCriteriaButton") || !findChild<QToolButton*>("primaryButton")->isHidden())) return fail(__LINE__);
    Language::setCurrent("tr"); translate(); QApplication::processEvents();
    if(!similarityThreshold_->prefix().contains("Benzerlik") || !similarityThreshold_->toolTip().contains("aday")) return fail(__LINE__);
    Language::setCurrent("en"); translate(); QApplication::processEvents();
    if(!similarityThreshold_->prefix().contains("Similarity")) return fail(__LINE__);
    similarityThreshold_->setValue(86); QApplication::processEvents();
    if(std::lround(options_.minFrameSimilarity)!=86) return fail(__LINE__);

    bool sourcesDialogOk=false;
    QTimer::singleShot(0,this,[&] {
        auto dialog=findChild<QDialog*>("scanSourcesDialog");
        if(!dialog) return;
        auto tree=dialog->findChild<QTreeWidget*>("includedSourcesTree");
        auto addFiles=dialog->findChild<QPushButton*>("addScanFilesButton");
        if(const auto shots=qEnvironmentVariable("COMPAREEL_CHECK_SHOTS"); !shots.isEmpty()) dialog->grab().save(shots+"/scan-sources.png");
        sourcesDialogOk=tree && tree->columnCount()==3 && addFiles && dialog->findChild<QListWidget*>("excludedFoldersList") && !dialog->findChild<QLineEdit*>("excludedExtensionsEdit");
        dialog->reject();
    });
    chooseFolders();
    if(!sourcesDialogOk) return fail(__LINE__);

    profile_->setCurrentIndex(2);
    const bool original=options_.matchResolution;
    for(bool save:{false,true}) {
        bool opened=false;
        QTimer::singleShot(0,this,[&] {
            auto dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if(!dialog) return;
            if(const auto shots=qEnvironmentVariable("COMPAREEL_CHECK_SHOTS"); !shots.isEmpty()) dialog->grab().save(shots+"/advanced-settings.png");
            for(auto check:dialog->findChildren<QCheckBox*>()) if(check->text()==text("Same resolution")) {
                opened=true; check->setChecked(!original);
            }
            if(auto fn=dialog->findChild<QCheckBox*>("matchFileNameCheck")) fn->setChecked(save);
            if(auto ex=dialog->findChild<QCheckBox*>("matchExtensionCheck")) ex->setChecked(save);
            if(auto th=dialog->findChild<QDoubleSpinBox*>("advancedSimilarityThreshold")) th->setValue(91);
            if(auto excl=dialog->findChild<QLineEdit*>("advancedExcludedExtensionsEdit")) excl->setText("gif, svg");
            auto buttons=dialog->findChild<QDialogButtonBox*>();
            if(buttons) buttons->button(save?QDialogButtonBox::Save:QDialogButtonBox::Cancel)->click();
            else dialog->reject();
        });
        // activated also fires when the user reselects the already active profile.
        QMetaObject::invokeMethod(profile_,"activated",Qt::DirectConnection,Q_ARG(int,2));
        QApplication::processEvents();
        if(!opened || options_.matchResolution!=(save?!original:original)) return fail(__LINE__);
    }
    if(!options_.matchFileName || !options_.matchExtension || similarityThreshold_->value()!=91 || options_.excludedExtensions.size()!=2) return fail(__LINE__);
    const auto criteria=activeCriteria();
    if(criteria.matchContent || criteria.matchVisualSamples || criteria.matchResolution!=options_.matchResolution || !criteria.matchFileName || !criteria.matchExtension) return fail(__LINE__);
    qInfo()<<"PASS criteria: inline similarity threshold visibility/sync/EN-TR tooltip, scan sources files/subfolders/excluded-extensions UI, select/reselect opens settings, Cancel preserves rules, Save updates custom rules (including filename/extension); no media";
    return true;
}

bool MainWindow::runImageCheck() {
    auto fail=[](const char* message) { qCritical("FAIL image UI: %s",message); return false; };
    QTemporaryDir fixture; if(!fixture.isValid()) return fail("private root");
    const auto media=fixture.filePath("images"); if(!QDir().mkpath(media)) return fail("private image folder");
    QImage image(128,96,QImage::Format_RGB32);
    for(int y=0;y<96;++y) for(int x=0;x<128;++x) image.setPixel(x,y,qRgb(x*2,y*2,(x+y)%256));
    { QPainter painter(&image); painter.fillRect(0,0,37,31,Qt::red); painter.fillRect(83,63,45,33,Qt::blue); }
    const auto first=QDir(media).filePath("0.png"),copy=QDir(media).filePath("1.png"),jpeg=QDir(media).filePath("2.jpg"),webp=QDir(media).filePath("3.webp");
    if(!image.save(first,"PNG") || !QFile::copy(first,copy) || !image.save(jpeg,"JPEG",98) || !image.save(webp,"WEBP",100)) return fail("generated PNG/JPEG/WebP fixture");
    image.fill(Qt::green); if(!image.save(QDir(media).filePath("4.png"),"PNG")) return fail("negative fixture");
    mediaMode_->setCurrentIndex(1); folders_={QDir::toNativeSeparators(media).toStdWString()}; options_.minFileSize=1; options_.crossFolderOnly=false;
    options_.protectedFolders.clear(); options_.excludedFolders.clear(); options_.extensions.clear(); options_.sortOrder=SortOrder::NameAsc; options_.minFrameSimilarity=90;
    sort_->setCurrentIndex(int(SortOrder::NameAsc)); profile_->setCurrentIndex(1); startScan();
    auto wait=[&](const auto& condition) { QElapsedTimer clock; clock.start(); while(!condition() && clock.elapsed()<20000) { QApplication::processEvents(QEventLoop::AllEvents,10); QThread::msleep(1); } return condition(); };
    if(!wait([&] { return !scanning_ && left_->canvas->frame() && right_->canvas->frame(); }) || groups_.size()!=1 || groups_[0].fileIndices.size()!=4) return fail("actual image similarity scan");
    if(!options_.images || scanCriteria_.matchVisualSamples || scanCriteria_.matchDuration || !scanCriteria_.matchFirstFrame || !left_->findChild<QWidget*>("playerTransport")->isHidden() || !offset_->parentWidget()->isHidden() || !findChild<QWidget*>("playbackBridge")->isHidden()) return fail("image mode/hidden video controls");
    if(auto model=qobject_cast<QStandardItemModel*>(sort_->model())) if(model->item(2)->isEnabled() || model->item(3)->isEnabled()) return fail("image duration sorting disabled");
    const auto manifest=fixture.filePath("image-job");
    { auto criteria=activeCriteria(); std::vector<VideoFileInfo> restored;
      if(!ScanManifest::save(manifest,criteria,files_) || !ScanManifest::load(manifest,criteria,restored) || restored.size()!=files_.size()) return fail("image frozen-list context");
      criteria.images=false; if(ScanManifest::load(manifest,criteria,restored)) return fail("video must not resume an image list"); }
    selectGroup(0,2); if(!wait([&] { return right_->canvas->frame()!=nullptr; }) || right_->fileIndex!=groups_[0].fileIndices[2]) return fail("third image selection");
    updateSimilarity(); if(!similarity_->toolTip().contains(text("Visually %1% similar · size, format or metadata may differ").section("%1",0,0))) return fail("image evidence label");
    QWheelEvent wheel(QPointF(30,30),left_->canvas->mapToGlobal(QPoint(30,30)),QPoint(),QPoint(0,120),Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);
    QApplication::sendEvent(left_->canvas,&wheel);
    if(left_->canvas->zoom()<=1 || left_->canvas->zoom()!=right_->canvas->zoom()) return fail("shared image zoom");
    const auto session=fixture.filePath("images.cvsession"); if(!saveSessionFile(session)) return fail("image session save");
    { QFile saved(session); if(!saved.open(QIODevice::ReadOnly)) return fail("read saved image session"); const auto doc=QJsonDocument::fromJson(saved.readAll()).object();
      if(doc["version"].toInt()!=3 || !doc["images"].toBool() || doc["groups"].toArray().empty() || doc["view"].toObject()["left"].toString().isEmpty() || doc["view"].toObject()["right"].toString().isEmpty()) return fail("image session version/active paths"); }
    {
        MainWindow restored(fixture.filePath("restored-state")); restored.show();
        if(!restored.loadSessionFile(session) || !wait([&] { return !restored.scanning_ && restored.left_->canvas->frame() && restored.right_->canvas->frame(); }) || !restored.options_.images || restored.groups_.size()!=1 || restored.groups_[0].rightSlot!=2 || restored.profile_->currentIndex()!=1) return fail("actual image session restore/active third");
        QFile input(session); if(!input.open(QIODevice::ReadOnly)) return fail("tamper fixture input");
        auto altered=QJsonDocument::fromJson(input.readAll()).object(); altered["version"]=1; altered["images"]=false;
        const auto tampered=fixture.filePath("downgraded.cvsession"); QFile output(tampered);
        if(!output.open(QIODevice::WriteOnly) || output.write(QJsonDocument(altered).toJson())<0) return fail("tamper fixture output"); output.close();
        if(restored.loadSessionFile(tampered) || !restored.options_.images || restored.groups_.size()!=1) return fail("image downgrade rejected without mutating active review");
        restored.privacyAction_->trigger(); if(!restored.hideThumbnailsAction_->isChecked() || !restored.hideVideosAction_->isChecked()) return fail("image privacy controls");
        for(int slot=0;slot<int(restored.groups_[0].fileIndices.size());++slot) if(!restored.thumbnailView(0,slot).previewHidden) return fail("hidden image thumbnails");
        restored.left_->setFile(nullptr,-1); restored.right_->setFile(nullptr,-1); restored.previewPool_.waitForDone();
        restored.left_->player->Close(); restored.right_->player->Close();
    }
    profile_->setCurrentIndex(0); startScan();
    if(!wait([&] { return !scanning_ && left_->canvas->frame() && right_->canvas->frame(); }) || groups_.size()!=1 || groups_[0].fileIndices.size()!=2 || !similarity_->toolTip().contains(text("Exact copy · 100% identical file contents")) || cacheHits_<5) return fail("exact image copies/SHA/warm cache");
    openFiles({first,webp}); if(!wait([&] { return left_->canvas->frame() && right_->canvas->frame(); }) || !manual_ || !options_.images) return fail("manual mixed image formats");
    const auto target=files_[left_->fileIndex]; const auto destination=fixture.filePath("moved.png");
    startTransfer(target,destination,"move");
    if(!wait([&] { return !fileOperationPending_; }) || QFileInfo::exists(first) || !QFileInfo::exists(destination) || !groups_.empty()) return fail("image Move closes image readers/regroups");
    QTimer answer; answer.setInterval(5); connect(&answer,&QTimer::timeout,this,[] {
        if(auto question=qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) if(auto yes=question->button(QMessageBox::Yes)) yes->click();
    });
    answer.start(); undoTransferAction_->trigger();
    if(!wait([&] { return !fileOperationPending_ && left_->canvas->frame() && right_->canvas->frame(); }) || !QFileInfo::exists(first) || QFileInfo::exists(destination) || groups_.size()!=1) return fail("image Move Undo/manual pair restoration");
    answer.stop();
    // Verify filename-only candidate matching (same stem "0" across 0.png and 0.bmp with distinct visual content)
    if(!image.save(QDir(media).filePath("0.bmp"),"BMP")) return fail("BMP fixture");
    options_.matchSize=false; options_.matchFirstFrame=false; options_.matchVisualSamples=false;
    options_.matchFileName=true; options_.matchExtension=false;
    { QSignalBlocker b(profile_); profile_->setCurrentIndex(2); }
    startScan();
    if(!wait([&] { return !scanning_ && left_->canvas->frame() && right_->canvas->frame(); }) || groups_.size()!=1 || !similarity_->toolTip().contains("Filename candidate")) { qCritical("groups=%d verdict: %s",int(groups_.size()),qPrintable(similarity_->toolTip())); return fail("filename candidate UI label"); }
    options_.matchFileName=false;
    left_->setFile(nullptr,-1); right_->setFile(nullptr,-1); previewPool_.waitForDone(); left_->player->Close(); right_->player->Close();
    qInfo()<<"PASS image UI: generated PNG/JPEG/WebP/BMP similar group, exact SHA/warm cache, filename candidate label, third image, shared wheel zoom, hidden video controls/duration sort, v3 saved-results session/restart and frozen list, video-context/downgrade refusal, privacy, manual pair, actual verified Move and Undo with reader release; no screenshots or user files";
    return true;
}

bool MainWindow::runHeifUiCheck(const QStringList& sources) {
    auto fail=[](const char* why) { qCritical()<<"HEIF UI:"<<why; return false; };
    if(sources.size()!=2) return fail("HEIC and AVIF fixture arguments");
    QTemporaryDir root; if(!root.isValid()) return fail("fixture root");
    for(auto pair:{std::pair{sources[0],"0.heic"},std::pair{sources[0],"1.heif"},std::pair{sources[1],"2.avif"},std::pair{sources[1],"3.avif"}})
        if(!QFile::copy(pair.first,root.filePath(pair.second))) return fail("private fixture copies");
    auto wait=[&](const auto& ready) { QElapsedTimer t; t.start(); while(!ready() && t.elapsed()<20000) { QApplication::processEvents(); QThread::msleep(2); } return ready(); };
    mediaMode_->setCurrentIndex(1); options_.minFileSize=1; options_.extensions.clear(); options_.protectedFolders.clear(); options_.crossFolderOnly=false;
    folders_={root.path().toStdWString()}; profile_->setCurrentIndex(0); startScan();
    if(!wait([&] { return !scanning_; }) || files_.size()!=4 || groups_.size()!=2 || !scanIssues_.empty()) return fail("discovery/exact groups");
    profile_->setCurrentIndex(1); startScan();
    if(!wait([&] { return !scanning_; }) || !scanIssues_.empty()) return fail("visual analysis");
    for(const auto& f:files_) if(!f.frameLoaded || !f.sigFirst.valid || (f.codec!=L"HEIC" && f.codec!=L"AVIF")) return fail("actual still evidence");
    startScan(); if(!wait([&] { return !scanning_; }) || cacheHits_!=4) return fail("warm cache");
    const auto session=root.filePath("heif-review.cvsession");
    if(!saveSessionFile(session) || !loadSessionFile(session) || !wait([&] { return !scanning_; }) || files_.size()!=4) return fail("image v2 session");
    openFiles({root.filePath("0.heic"),root.filePath("2.avif")});
    if(!wait([&] { return left_->canvas->frame() && right_->canvas->frame(); }) || !options_.images || !manual_) return fail("manual mixed-format pair");
    qInfo()<<"PASS HEIC/HEIF/AVIF actual UI: local public fixture copies, discovery, exact/visual evidence, warm cache, v2 session, manual pair and full previews; no screenshots or user media";
    return true;
}

bool MainWindow::runImageGuardCheck(const QStringList& sources) {
    auto fail=[](const char* reason) { qCritical()<<"Image guard:"<<reason; return false; };
    if(sources.size()!=2) return fail("need generated GIF and TIFF");
    auto wait=[&](auto ready,int ms=10000) { QElapsedTimer t; t.start(); while(!ready() && t.elapsed()<ms) { QApplication::processEvents(); QThread::msleep(2); } return ready(); };
    Language::setCurrent("en"); translate(); hideVideosAction_->setChecked(false); applyPrivacy();
    QTemporaryDir root; const auto gif=root.filePath("copy.gif"); if(!QFile::copy(sources[0],gif)) return fail("fixture copy");
    openFiles({sources[0],gif});
    if(!wait([&] { return left_->animationReady() && right_->animationReady(); })) return fail("animation pair ready");
    tick();
    if(left_->imagePlaying() || right_->imagePlaying() || left_->imageFrameIndex()!=0 || right_->imageFrameIndex()!=0) return fail("initially still");
    // Animations get play/pause, frame step and a time bar, like page documents.
    if(!left_->findChild<QWidget*>("playerTransport")->isVisible() || left_->findChild<QLabel*>("animationFrameCount")->text()!="3 frames") return fail("visible transport and total count");
    auto mouse=[&](VideoCanvas* canvas,bool down) { QMouseEvent event(down?QEvent::MouseButtonPress:QEvent::MouseButtonRelease,QPointF(10,10),QPointF(canvas->mapToGlobal(QPoint(10,10))),Qt::LeftButton,down?Qt::LeftButton:Qt::NoButton,Qt::NoModifier); QApplication::sendEvent(canvas,&event); };
    for(auto panel:{left_,right_}) {
        mouse(panel->canvas,true);
        if(!left_->imagePlaying() || !right_->imagePlaying()) return fail("either hold starts both");
        if(!wait([&] { tick(); return left_->imageFrameIndex()>0 && right_->imageFrameIndex()>0; })) return fail("hold advances both");
        mouse(panel->canvas,false);
        if(left_->imagePlaying() || right_->imagePlaying() || left_->imageFrameIndex()!=0 || right_->imageFrameIndex()!=0) return fail("release resets both");
    }
    mouse(left_->canvas,true); QFocusEvent lostFocus(QEvent::FocusOut); QApplication::sendEvent(left_->canvas,&lostFocus);
    if(left_->imagePlaying() || right_->imagePlaying()) return fail("focus loss stops hold");
    mouse(right_->canvas,true); hideVideosAction_->setChecked(true); applyPrivacy();
    if(left_->imagePlaying() || right_->imagePlaying()) return fail("privacy pause");
    mouse(right_->canvas,false); mouse(left_->canvas,true);
    if(left_->imagePlaying() || right_->imagePlaying()) return fail("privacy blocks hold");
    mouse(left_->canvas,false);
    const auto tiff=root.filePath("copy.tif"); if(!QFile::copy(sources[1],tiff)) return fail("TIFF copy");
    hideVideosAction_->setChecked(false); applyPrivacy(); openFiles({sources[1],tiff});
    if(!wait([&] { return left_->imageDocument() && right_->imageDocument(); })) return fail("document pages");
    auto first=left_->canvas->frame(); left_->findChild<QSlider*>("playerSeek")->setValue(1);
    if(!wait([&] { return left_->canvas->frame()!=first && left_->imageFrameIndex()==1; }) || left_->imagePlaying()) return fail("actual second page");
    std::vector<QString> names;
    for(int i=0;i<6;++i) { const auto name=root.filePath(QString::number(i)+".png"); QImage im(32+i*8,32,QImage::Format_RGB32); im.fill(QColor(i*30,50,100)); if(!im.save(name)) return fail("PNG"); names.push_back(name); }
    left_->setFile(nullptr,-1); right_->setFile(nullptr,-1); files_.clear(); groups_.clear(); currentGroup_=-1; manual_=true;
    for(int i=0;i<6;++i) { VideoFileInfo f; f.fullPath=names[i].toStdWString(); f.fileName=QFileInfo(names[i]).fileName().toStdWString(); f.identity=FileOperations::ReadIdentity(f.fullPath); f.fileSize=uint64_t(i+1)*100; f.width=32+i*8; f.height=32; f.metadataLoaded=true; files_.push_back(f); }
    VideoGroup a,b; a.fileIndices={0,1,2}; a.rightSlot=1; b.fileIndices={3,4,5}; b.rightSlot=2; groups_={a,b}; populateResults(); selectGroup(1,2); files_[3].protectedFile=true;
    auto expectedSpace=[](uint64_t total,uint64_t other,bool exact=false) { return text(exact?"Total: %1 · Potential savings: %2":"Total: %1 · Extra copies: %2").arg(QString::fromStdWString(FormatBytesShort(total)),QString::fromStdWString(FormatBytesShort(other))); };
    if(spaceSummary_->text()!=expectedSpace(2100,1600)) return fail("result sizes before left preference");
    // Protected files stay the reference; otherwise the largest file moves to the left.
    referenceRule_->setCurrentIndex(referenceRule_->findData(int(ReferenceRule::LargestFile)));
    if(currentGroup_!=1 || groups_[0].fileIndices[0]!=2 || groups_[1].fileIndices[0]!=3 || left_->fileIndex!=3 || !files_[3].protectedFile) return fail("reference largest/protected first/group preservation");
    // Kept references are files 2 (300) and the protected file 3 (400).
    if(spaceSummary_->text()!=expectedSpace(2100,1400)) return fail("result sizes follow the reference rule");
    const auto savedGroups=groups_;
    groups_.push_back(groups_[0]); populateResults();
    if(spaceSummary_->text()!=expectedSpace(2100,1400)) return fail("result sizes deduplicate group membership");
    files_[1].deleted=true; populateResults();
    if(spaceSummary_->text()!=expectedSpace(1900,1200)) return fail("result sizes exclude removed files");
    files_[1].deleted=false; groups_=savedGroups;
    const auto savedIdentity=files_[1].identity; files_[1].identity=files_[0].identity; files_[1].fileSize=files_[0].fileSize; populateResults();
    if(spaceSummary_->text()!=expectedSpace(1900,1200)) return fail("result sizes deduplicate physical identity");
    files_[1].identity=savedIdentity; files_[1].fileSize=200;
    for(auto& file:files_) file.contentHash="verified-fixture";
    for(auto& file:files_) file.fileSize=100;
    populateResults(); if(spaceSummary_->text()!=expectedSpace(600,400,true)) return fail("verified content savings label");
    for(int i=0;i<6;++i) { files_[i].contentHash.clear(); files_[i].fileSize=uint64_t(i+1)*100; }
    populateResults();
    files_[1].error=HRESULT_FROM_WIN32(ERROR_INVALID_DATA); files_[2].error=HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    const auto diagnostic=scanReportText();
    const auto reportPath=root.filePath("read-report.txt");
    if(!diagnostic.contains("2 read errors") || !diagnostic.contains("png · 0x8007000d: 2") || !saveScanReportFile(reportPath,diagnostic)) return fail("persistent scan error summary");
    QFile savedReport(reportPath); if(!savedReport.open(QIODevice::ReadOnly) || QString::fromUtf8(savedReport.readAll())!=diagnostic) return fail("scan report saved text round trip");
    files_[1].error=files_[2].error=S_OK;
    selectGroup(1,2); const int selectedRight=right_->fileIndex;
    for(auto& f:files_) f.metadataLoaded=false;
    referenceRule_->setCurrentIndex(referenceRule_->findData(int(ReferenceRule::HighestResolution)));
    if(!wait([&] { return !sortingMetadata_; })) return fail("resolution metadata read");
    if(currentGroup_!=1 || right_->fileIndex!=selectedRight || groups_[0].fileIndices[0]!=2) return fail("resolution/right candidate preservation");
    {
        // A hand-picked reference survives a rule change; the previous reference becomes the compared candidate.
        const int previous=groups_[0].fileIndices[0],chosen=groups_[0].fileIndices[2];
        makeReference(0,2);
        if(groups_[0].fileIndices[0]!=chosen || groups_[0].fileIndices[1]!=previous || right_->fileIndex!=previous || !groups_[0].manualReference) return fail("make reference");
        referenceRule_->setCurrentIndex(referenceRule_->findData(int(ReferenceRule::LargestFile)));
        if(groups_[0].fileIndices[0]!=chosen) return fail("manual reference kept across rule change");
        if(left_->findChild<QLabel*>("panelRole")->text()!=text("Reference") || !right_->findChild<QLabel*>("panelRole")->text().startsWith(text("Candidate"))) return fail("panel role captions");
        groups_[0].manualReference=false; selectGroup(1,2);
    }
    resize(1040,720); QApplication::processEvents();
    if(width()>1040 || sort_->mapTo(this,QPoint(sort_->width(),0)).x()>width()) { qCritical()<<"layout width"<<width(); return fail("compact width"); }
    manual_=false; scanCriteria_=ScanCriteria{}; scanCriteria_.images=true; scanCriteria_.matchSize=false; scanCriteria_.matchFirstFrame=true; scanCriteria_.matchTransforms=false; scanCriteria_.minFrameSimilarity=95;
    for(int i=0;i<3;++i) { files_[i].protectedFile=false; files_[i].frameLoaded=true; files_[i].sigFirst={}; files_[i].sigFirst.valid=true; files_[i].sigFirst.dHashH=i==0?0:i==1?1023:1048575; }
    VideoGroup related; related.fileIndices={0,1,2}; groups_={related};
    auto plan=cleanupPlan();
    if(plan.size()!=1 || plan[0].index!=1 || plan[0].keeper.fullPath!=files_[0].fullPath) return fail("indirect match excluded from cleanup plan");
    std::vector<VideoGroup> chain; std::atomic<bool> noCancel{false};
    VideoScanner::GroupKnownFiles(files_,{0,1,2},scanCriteria_,chain,noCancel);
    if(chain.size()!=1 || chain[0].fileIndices.size()!=3 || VideoScanner::FilesMatch(files_[0],files_[2],scanCriteria_)) return fail("related chain grouping preserves direct edges");
    groups_=chain; populateResults(); selectGroup(0);
    int endA=0,endC=0; for(size_t i=0;i<groups_[0].fileIndices.size();++i) { if(groups_[0].fileIndices[i]==0) endA=int(i); if(groups_[0].fileIndices[i]==2) endC=int(i); }
    std::swap(groups_[0].fileIndices[0],groups_[0].fileIndices[endA]);
    endC=int(std::find(groups_[0].fileIndices.begin(),groups_[0].fileIndices.end(),2)-groups_[0].fileIndices.begin());
    selectGroup(0,endC);
    if(!similarity_->text().contains("does not meet") || !similarity_->text().contains("93.2%")) return fail("indirect pair visible explanation and actual score");
    qInfo()<<"PASS actual UI: animation initially still, total count without transport, either mouse hold plays both, release/focus loss resets, privacy blocks hold; TIFF page navigation, reference rule (protected first, make reference survives rule change)/group stability/right choice/protection";
    return true;
}

bool MainWindow::runPartialSearchUiCheck(const QStringList& sources) {
    if(sources.size()!=1) return false;
    auto wait=[&](const auto& ready) { QElapsedTimer t; t.start(); while(!ready() && t.elapsed()<20000) { QApplication::processEvents(); QThread::msleep(2); } return ready(); };
    Language::setCurrent("en"); translate(); folders_={sources[0].toStdWString()}; options_.minFileSize=1; options_.crossFolderOnly=false; options_.protectedFolders.clear();
    bool seen=false,rows=false; unsigned ticks=0; QTimer observer;
    connect(&observer,&QTimer::timeout,this,[&] {
        ++ticks;
        if(auto dialog=findChild<QDialog*>("partialSearchResults")) {
            auto table=dialog->findChild<QTreeWidget*>("partialCandidates"); rows=table && table->topLevelItemCount()==1; seen=true;
            if(rows) dialog->findChild<QPushButton*>("openPartialPair")->click(); else dialog->reject();
        }
    }); observer.start(5);
    findChild<QAction*>("Find partial clips in folders…")->trigger();
    if(!wait([&] { return seen && !sortingMetadata_; }) || !rows || ticks<2 || !manual_ || files_.size()!=2 || !files_[0].protectedFile || !files_[1].protectedFile || !cleanupPlan().empty() || std::abs(std::abs(alignmentOffset100ns_)-60000000)>5000000) { qCritical()<<"Partial UI result/protection failed"<<seen<<rows<<ticks; return false; }
    if(!wait([&] { return !restorePositions_ && left_->player->IsLoaded() && right_->player->IsLoaded() && !left_->player->IsSeeking() && !right_->player->IsSeeking() && pendingPreviews_.empty(); })) return false;
    if(!files_[0].protectedFile || !files_[1].protectedFile || !cleanupPlan().empty()) return false;
    QTemporaryDir session; if(!session.isValid() || !saveSessionFile(session.filePath("partial.cvsession")) || !loadSessionFile(session.filePath("partial.cvsession")) || !wait([&] { return !scanning_; }) || !files_[0].protectedFile || !files_[1].protectedFile || !cleanupPlan().empty()) return false;
    observer.stop(); findPartialClips(); cancel_.store(true);
    if(!wait([&] { return !sortingMetadata_; })) return false;
    qInfo()<<"PASS actual partial-folder menu, async result, protected review pair, offset/seek, session protection and cancel; generated videos only, no screenshots";
    return true;
}

bool MainWindow::runAudioEvidenceUiCheck(const QStringList& sources) {
    if(sources.size()!=2) return false;
    auto wait=[&](const auto& ready) { QElapsedTimer timer; timer.start(); while(!ready() && timer.elapsed()<20000) { QApplication::processEvents(); QThread::msleep(2); } return ready(); };
    Language::setCurrent("en"); translate(); openFiles(sources);
    if(!wait([&] { return !scanning_ && left_->fileIndex>=0 && right_->fileIndex>=0; })) return false;
    auto action=findChild<QAction*>("Compare sampled audio"); if(!action) return false;
    bool seen=false,good=false; unsigned ticks=0; QTimer observer;
    connect(&observer,&QTimer::timeout,this,[&] {
        ++ticks;
        if(auto box=findChild<QMessageBox*>("audioEvidenceResult")) { seen=true; good=box->text().contains("Audio samples are similar") && box->text().contains("completed windows: 3"); if(!good) qCritical()<<box->text(); box->accept(); }
    });
    observer.start(5); action->trigger();
    if(!wait([&] { return seen && !sortingMetadata_; }) || !good || ticks<2) { qCritical()<<"Audio UI failed"<<seen<<good<<ticks; return false; }
    action->trigger(); cancel_.store(true);
    if(!wait([&] { return !sortingMetadata_; })) return false;
    qInfo()<<"PASS sampled audio actual menu, async responsive UI, result scope/three windows and cancellation; generated clips only, no screenshots";
    return true;
}

bool MainWindow::runEvidenceCheck() {
    auto fail=[](const char* why) { qCritical()<<"Evidence check:"<<why; return false; };
    auto wait=[&](const auto& ready) { QElapsedTimer t; t.start(); while(!ready() && t.elapsed()<20000) { QApplication::processEvents(); QThread::msleep(2); } return ready(); };
    QTemporaryDir root; if(!root.isValid()) return fail("fixture root");
    QImage image(128,96,QImage::Format_RGB32);
    for(int y=0;y<96;++y) for(int x=0;x<128;++x) image.setPixel(x,y,qRgb(x*2,y*2,(x+y)%256));
    const auto a=root.filePath("a.png"),b=root.filePath("b.png"),c=root.filePath("c.jpg"),d=root.filePath("d.png"),e=root.filePath("e.png");
    if(!image.save(a) || !QFile::copy(a,b) || !image.save(c,"JPEG",83)) return fail("first family");
    image.invertPixels(); if(!image.save(d) || !QFile::copy(d,e)) return fail("second family");
    mediaMode_->setCurrentIndex(1); folders_={root.path().toStdWString()}; options_.minFileSize=1; options_.crossFolderOnly=false; options_.matchTransforms=false; options_.protectedFolders.clear(); options_.sortOrder=SortOrder::NameAsc; sort_->setCurrentIndex(int(SortOrder::NameAsc));
    referenceRule_->setCurrentIndex(referenceRule_->findData(int(ReferenceRule::NameOrder)));
    profile_->setCurrentIndex(1); startScan();
    if(!wait([&] { return !scanning_ && !sortingMetadata_ && previewPool_.activeThreadCount()==0 && pendingPreviews_.empty(); }) || groups_.size()!=2) return fail("visual groups");
    if(proofLevel(groups_[0]) || proofLevel(groups_[1])) return fail("unhashed group called verified");
    findChild<QAction*>("Verify exact matches in results")->trigger();
    if(!wait([&] { return !sortingMetadata_; }) || proofLevel(groups_[0])!=1 || proofLevel(groups_[1])!=2) return fail("mixed versus full SHA proof");
    evidenceFilter_->setCurrentIndex(1);
    if(!results_->topLevelItem(0)->isHidden() || results_->topLevelItem(1)->isHidden()) return fail("full proof filter");
    evidenceFilter_->setCurrentIndex(2);
    if(results_->topLevelItem(0)->isHidden() || results_->topLevelItem(1)->isHidden()) return fail("mixed proof filter");
    evidenceFilter_->setCurrentIndex(3);
    if(results_->topLevelItem(0)->isHidden() || !results_->topLevelItem(1)->isHidden()) return fail("other candidates filter");
    evidenceFilter_->setCurrentIndex(0);
    const auto selected=groups_[currentGroup_].fileIndices;
    findChild<QAction*>("Exact-copy groups first")->trigger();
    if(proofLevel(groups_[0])!=2 || groups_[currentGroup_].fileIndices!=selected) return fail("proof ordering changed selected group");
    selectGroup(currentGroup_,1);
    if(!similarity_->toolTip().contains(text("Exact copy · 100% identical file contents"))) return fail("actual pair SHA label");
    selectGroup(currentGroup_,2);
    if(similarity_->toolTip().contains(text("Exact copy · 100% identical file contents"))) return fail("JPEG incorrectly labelled byte identical");
    qInfo()<<"PASS SHA evidence: actual visual scan + background same-size hashing, mixed/full proof, filters and ordering preserve selected group; JPEG not byte-identical; generated images";
    return true;
}

bool MainWindow::runCachePolicyCheck() {
    auto fail=[](const char* why) { qCritical()<<"Cache policy check:"<<why; return false; };
    auto wait=[&](const auto& ready) { QElapsedTimer t; t.start(); while(!ready() && t.elapsed()<20000) { QApplication::processEvents(); QThread::msleep(2); } return ready(); };
    QTemporaryDir fixture; if(!fixture.isValid()) return fail("fixture root");
    QImage image(64,64,QImage::Format_RGB32);
    for(int y=0;y<64;++y) for(int x=0;x<64;++x) image.setPixel(x,y,qRgb(x*4,y*4,(x+y)*2));
    const auto first=fixture.filePath("first.png"),second=fixture.filePath("second.png");
    if(!image.save(first) || !QFile::copy(first,second)) return fail("generated image pair");
    auto action=findChild<QAction*>("Store media cache on disk"); if(!action || !action->isChecked()) return fail("default on");
    mediaMode_->setCurrentIndex(1); folders_={fixture.path().toStdWString()}; options_.minFileSize=1; options_.crossFolderOnly=false; options_.protectedFolders.clear();
    profile_->setCurrentIndex(1);
    auto scan=[&] { startScan(); return wait([&] { return !scanning_ && previewPool_.activeThreadCount()==0 && pendingPreviews_.empty(); }) && groups_.size()==1; };
    if(!scan()) return fail("baseline image scan");
    const auto source=files_[0];
    { ScanCache cache(cachePath_); auto restored=source; if(!cache.restore(restored)) return fail("baseline cache missing"); cache.recordRemoval(source,E_ACCESSDENIED); }
    action->trigger(); if(!wait([&] { return !clearingCache_; }) || action->isChecked()) return fail("disable UI");
    { ScanCache cache(cachePath_); auto restored=source; if(cache.mediaCachingEnabled() || cache.restore(restored) || cache.operationHistory().size()!=1) return fail("restart policy/history"); }
    {
        auto db=QSqlDatabase::addDatabase("QSQLITE","policy-fixture"); db.setDatabaseName(cachePath_);
        if(!db.open()) return fail("cache inspect");
        QSqlQuery query(db); if(!query.exec("SELECT count(*) FROM scan_cache_v4") || !query.next() || query.value(0).toInt()!=0) return fail("old media rows remain");
        query.finish(); db.close();
    }
    QSqlDatabase::removeDatabase("policy-fixture");
    if(!scan() || cacheHits_!=0) return fail("visual scan without disk cache");
    profile_->setCurrentIndex(0); if(!scan()) return fail("SHA scan without disk cache");
    { ScanCache cache(cachePath_); for(auto f:files_) if(cache.restore(f)) return fail("disabled scan wrote evidence"); }
    {
        MainWindow restarted(QFileInfo(settings_.fileName()).absolutePath());
        if(restarted.findChild<QAction*>("Store media cache on disk")->isChecked()) return fail("UI policy did not persist");
    }
    action->trigger(); if(!wait([&] { return !clearingCache_; }) || !action->isChecked() || !scan()) return fail("reenable cache");
    { ScanCache cache(cachePath_); auto f=files_[0]; if(!cache.restore(f) || cache.operationHistory().size()!=1) return fail("reenabled evidence/history"); }
    qInfo()<<"PASS media cache policy: default on, actual UI off/clear, visual and SHA scans without disk evidence, restart, reenable and durable history; generated images only";
    return true;
}

bool MainWindow::runPathCheck(const QString& mediaPath) {
    auto fail=[](const char* message) { qCritical("FAIL paths: %s",message); return false; };
    QTemporaryDir fixture; if(!fixture.isValid()) return fail("private root");
    QString root=fixture.path();
    for(int i=0;i<5;++i) root+=QString::fromUtf8("/Türkçe İı şğ 資料 ")+QString(36,'x')+QString::number(i);
    if(!QDir().mkpath(root)) return fail("long Unicode directory");
    const auto a=root+QString::fromUtf8("/çağrı 影片 A.mp4"),b=root+QString::fromUtf8("/çağrı 影片 B.mp4");
    if(a.size()<=300 || !QFile::copy(mediaPath,a) || !QFile::copy(mediaPath,b)) return fail("private long-path copies");
    const auto original=FileOperations::ReadIdentity(QFileInfo(mediaPath).absoluteFilePath().toStdWString());
    folders_={QDir::toNativeSeparators(fixture.path()).toStdWString()};
    options_.minFileSize=1; options_.crossFolderOnly=false; options_.protectedFolders.clear(); options_.excludedFolders.clear();
    auto ready=[&] { return !scanning_ && !restorePositions_ && left_->player->IsLoaded() && right_->player->IsLoaded() && !left_->player->IsSeeking() && !right_->player->IsSeeking(); };
    auto wait=[&](const auto& condition) { QElapsedTimer clock; clock.start(); while(!condition() && clock.elapsed()<20000) { QApplication::processEvents(QEventLoop::AllEvents,20); QThread::msleep(2); } return condition(); };
    profile_->setCurrentIndex(0); startScan();
    if(!wait(ready) || files_.size()!=2 || groups_.size()!=1 || !scanIssues_.empty()) {
        qCritical()<<"Path check state: scanning"<<scanning_<<"files"<<files_.size()<<"groups"<<groups_.size()<<"issues"<<scanIssues_.size()<<"players"<<quint32(left_->player->GetError())<<quint32(right_->player->GetError());
        for(const auto& issue:scanIssues_) qCritical()<<"Discovery HRESULT"<<quint32(issue.error);
        for(const auto& file:files_) qCritical()<<"File identity/hash/error"<<file.identity.valid<<file.contentHash.size()<<quint32(file.error);
        return fail("discover/hash/paired playback");
    }
    for(const auto& file:files_) if(!file.identity.valid || file.contentHash.size()!=64 || file.fullPath.size()<=300) return fail("long-path identity/hash");
    profile_->setCurrentIndex(1); startScan();
    if(!wait(ready) || groups_.size()!=1 || !scanIssues_.empty()) return fail("visual scan");
    for(const auto& file:files_) if(!file.frameLoaded || file.samples.size()!=5) return fail("five samples");
    startScan(); if(!wait(ready) || cacheHits_!=2 || groups_.size()!=1) return fail("warm cache");
    const auto session=root+QString::fromUtf8("/inceleme 資料.cvsession");
    if(!saveSessionFile(session) || !loadSessionFile(session) || !wait(ready) || files_.size()!=2 || groups_.size()!=1) return fail("Unicode long session round trip");
    const auto target=left_->player->GetDuration100ns()/2;
    controlSeek(left_,target,true);
    if(!wait([&] { return !left_->player->IsSeeking() && !right_->player->IsSeeking(); }) || std::abs(left_->player->GetPosition100ns()-target)>1000000 || std::abs(right_->player->GetPosition100ns()-target)>1000000) return fail("linked seek");
    left_->setFile(nullptr,-1); right_->setFile(nullptr,-1); previewPool_.waitForDone(); left_->player->Close(); right_->player->Close();
    const auto path=QDir::toNativeSeparators(b).toStdWString(); const auto identity=FileOperations::ReadIdentity(path);
    if(!identity.valid) return fail("private-copy identity before Trash");
    const auto removed=FileOperations::Remove(nullptr,path,true,identity);
    if(!identity.valid || !removed.succeeded() || QFileInfo::exists(b) || !QFileInfo::exists(a)) {
        qCritical()<<"Long-path Trash HRESULT"<<quint32(removed.error)<<"identity"<<identity.valid<<"target exists"<<QFileInfo::exists(b);
        return fail("private-copy long-path Recycle Bin");
    }
    if(!original.valid || FileOperations::ReadIdentity(QFileInfo(mediaPath).absoluteFilePath().toStdWString())!=original) return fail("source changed");
    qInfo()<<"PASS paths:"<<a.size()<<"characters, Unicode/spaces, discover/SHA/five samples/warm cache/session/paired playback/linked seek/private-copy Trash; source unchanged; no screenshots";
    return true;
}

void MainWindow::runCrashScanChild(const QString& folder,bool freezeList) {
    folders_={QDir::toNativeSeparators(QFileInfo(folder).absoluteFilePath()).toStdWString()};
    options_.minFileSize=1; options_.crossFolderOnly=false; profile_->setCurrentIndex(0);
    if(freezeList) {
        const auto criteria=activeCriteria();
        const auto files=VideoScanner::DiscoverVideos(folders_,true,cancel_,nullptr,&criteria);
        if(files.empty() || !ScanManifest::save(cachePath_+".scan-job",criteria,files)) std::_Exit(74);
        saveSettings(); settings_.setValue("scanInProgress",true); settings_.sync();
        std::_Exit(73);
    }
    startScan();
    // Deliberate self-check crash after the real scan launch, bypassing closeEvent and destructors.
    std::_Exit(scanning_ && settings_.value("scanInProgress").toBool()?73:74);
}

bool MainWindow::runContinuationCheck(const QString& mediaPath) {
    auto fail=[](const char* message) { qCritical("FAIL continuation: %s",message); return false; };
    QTemporaryDir fixture; if(!fixture.isValid()) return fail("temporary directory");
    const auto source=QFileInfo(mediaPath).absoluteFilePath();
    const auto root=fixture.filePath("media"),state=fixture.filePath("state");
    if(!QDir().mkpath(root)) return fail("fixture folder");
    for(auto name:{"a.mp4","b.mp4"}) if(!QFile::copy(source,QDir(root).filePath(name))) return fail("private fixture copy");
    std::atomic<bool> cancel{false}; ScanCriteria criteria; std::vector<VideoFileInfo> snapshot;
    QString manifest;
    QProcess child;
    child.start(QCoreApplication::applicationFilePath(),{"--crash-scan-child","--freeze-list","--media",root,"--data-dir",state});
    if(!child.waitForFinished(10000) || child.exitCode()!=73) return fail("abrupt checkpoint child");
    if(!QFile::copy(source,QDir(root).filePath("later.mp4"))) return fail("new arrival fixture");
    MainWindow resumed(state);
    criteria=resumed.activeCriteria(); snapshot=VideoScanner::DiscoverVideos(resumed.folders_,false,cancel,nullptr,&criteria);
    std::erase_if(snapshot,[](const auto& file) { return file.fileName==L"later.mp4"; });
    manifest=resumed.cachePath_+".scan-job";
    if(snapshot.size()!=2 || !QFileInfo::exists(manifest)) return fail("checkpoint after abrupt exit");
    if(!resumed.pausedScan_ || !resumed.resumeAction_->isEnabled()) return fail("restart resume action");
    auto wait=[&] {
        QElapsedTimer timer; timer.start();
        while(resumed.scanning_ && timer.elapsed()<10000) { QApplication::processEvents(QEventLoop::AllEvents,20); QThread::msleep(1); }
        return !resumed.scanning_;
    };
    resumed.resumeAction_->trigger();
    if(!wait() || resumed.files_.size()!=2 || resumed.groups_.size()!=1 || QFileInfo::exists(manifest)) return fail("frozen list must exclude new arrivals and clear on completion");
    resumed.left_->setFile(nullptr,-1); resumed.right_->setFile(nullptr,-1); resumed.previewPool_.waitForDone();
    if(!ScanManifest::save(manifest,criteria,snapshot)) return fail("second checkpoint");
    { QFile changed(QDir(root).filePath("b.mp4")); if(!changed.open(QIODevice::Append) || changed.write("changed")!=7) return fail("changed private member"); }
    resumed.pausedScan_=true; resumed.resumeAction_->setEnabled(true); resumed.resumeAction_->trigger();
    if(!wait() || resumed.files_.size()!=2 || resumed.scanIssues_.size()!=1 || !resumed.groups_.empty()) return fail("changed member excluded");
    const auto test=fixture.filePath("invalid.scan-job");
    if(!ScanManifest::save(test,criteria,snapshot)) return fail("validation fixture");
    auto altered=criteria; altered.minFrameSimilarity-=1; auto preserved=snapshot;
    if(ScanManifest::load(test,altered,preserved) || preserved.size()!=2) return fail("criteria mismatch changed destination");
    QFile input(test); if(!input.open(QIODevice::ReadOnly)) return fail("validation read");
    auto object=QJsonDocument::fromJson(input.readAll()).object(); input.close();
    auto rows=object["files"].toArray(); auto first=rows[0].toArray(); first[0]=fixture.filePath("outside.mp4"); rows[0]=first; object["files"]=rows;
    { QFile output(test); if(!output.open(QIODevice::WriteOnly) || output.write(QJsonDocument(object).toJson())<0) return fail("validation write"); }
    if(ScanManifest::load(test,criteria,preserved) || preserved.size()!=2) return fail("out-of-root list accepted");
    if(!ScanManifest::save(manifest,criteria,snapshot)) return fail("clear-cache checkpoint fixture");
    { ScanCache cache(resumed.cachePath_); cache.clearScanData(); if(!cache.error().isEmpty() || QFileInfo::exists(manifest)) return fail("clear cache left frozen paths"); }
    qInfo("PASS continuation: abrupt child exit73/restart/resume uses frozen list, new arrival excluded, changed member excluded, successful job cleared, changed criteria and outside-root paths rejected, clear cache removes frozen paths; no screenshot");
    return true;
}

bool MainWindow::runTimingCheck(const QString& mediaPath,const QString& screenshotPath) {
    auto wait=[](const std::function<bool()>& ready) {
        QElapsedTimer elapsed; elapsed.start();
        while(!ready() && elapsed.elapsed()<10000) { QApplication::processEvents(); QThread::msleep(10); }
        return ready();
    };
    auto fail=[](const char* reason) { qWarning()<<"Timing check failed:"<<reason; return false; };
    if(scanDuration(1000LL*(100*3600+5*60+7))!="100:05:07") return fail("hour wrap");
    QDir().mkpath(QFileInfo(screenshotPath).absolutePath());
    QTemporaryDir fixture(QFileInfo(screenshotPath).absolutePath()+"/timing-files-XXXXXX");
    if(!fixture.isValid()) return fail("fixture directory");
    for(int i=0;i<4;++i) if(!QFile::copy(mediaPath,fixture.filePath(QString("Copy-%1.mp4").arg(i)))) return fail("fixture copy");
    folders_={QDir::toNativeSeparators(fixture.path()).toStdWString()}; options_.minFileSize=1;
    options_.protectedFolders.clear(); options_.excludedFolders.clear(); profile_->setCurrentIndex(0);
    startScan();
    if(!scanning_ || !scanElapsed_.isValid() || !scanTimeLabel_->text().startsWith(text("Elapsed: %1").arg(""))) return fail("live timer");
    if(!wait([&] { return !scanning_ && pendingPreviews_.empty(); }) || lastScanOutcome_!="completed" || scanElapsed_.isValid()) return fail("completion");
    auto label=scanTimeLabel_->text(); auto saved=lastScanElapsedMs_;
    statusBar()->showMessage("A temporary player/status message"); QApplication::processEvents();
    if(scanTimeLabel_->isHidden() || scanTimeLabel_->text()!=label || settings_.value("lastScanElapsedMs").toLongLong()!=saved) return fail("permanent label/save");
    { MainWindow reopened(QFileInfo(settings_.fileName()).absolutePath());
      if(reopened.lastScanOutcome_!="completed" || reopened.lastScanElapsedMs_!=saved || reopened.scanTimeLabel_->text()!=label) return fail("reopen"); }
    startScan(); stopAction_->trigger();
    if(!wait([&] { return !scanning_; }) || lastScanOutcome_!="stopped") return fail("stop");
    startScan(); pauseAction_->trigger();
    if(!wait([&] { return !scanning_; }) || lastScanOutcome_!="paused") return fail("pause");
    resumeAction_->trigger();
    if(!wait([&] { return !scanning_ && pendingPreviews_.empty(); }) || lastScanOutcome_!="completed" || !wait([&] { return left_->player->IsLoaded() && right_->player->IsLoaded(); })) {
        qWarning()<<"resume state: outcome"<<lastScanOutcome_<<"scanning"<<scanning_<<"sortingMetadata"<<sortingMetadata_<<"pendingPreviews"<<pendingPreviews_.size()<<"groups"<<groups_.size()
                  <<"left"<<left_->fileIndex<<left_->player->IsLoaded()<<"right"<<right_->fileIndex<<right_->player->IsLoaded();
        return fail("resume");
    }
    auto crashState=fixture.filePath("crash-state"); QProcess child;
    child.start(QCoreApplication::applicationFilePath(),{"--crash-scan-child","--media",fixture.path(),"--data-dir",crashState});
    if(!child.waitForFinished(10000) || child.exitCode()!=73) return fail("abrupt actual scan child");
    {
        MainWindow restarted(crashState);
        if(!restarted.pausedScan_ || !restarted.resumeAction_->isEnabled() || restarted.folders_.size()!=1) return fail("interrupted scan offered after restart");
        restarted.resumeAction_->trigger();
        if(!wait([&] { return !restarted.scanning_; }) || restarted.groups_.size()!=1 || restarted.files_.size()!=4 || restarted.settings_.value("scanInProgress").toBool()) return fail("actual restarted scan completion");
    }
    if(!grab().save(screenshotPath)) return fail("screenshot");
    left_->setFile(nullptr,-1); right_->setFile(nullptr,-1); left_->player->Close(); right_->player->Close(); previewPool_.waitForDone();

    qInfo()<<"PASS timing: actual scan/live/completed, permanent status label, reopen persistence, stop, pause/cache resume, abrupt child scan/restart/resume, 100-hour formatting";
    return true;
}
