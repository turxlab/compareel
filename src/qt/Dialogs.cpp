#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "ScanDialogs.h"
#include "Language.h"
#include "Icons.h"
#include "Theme.h"
#include "FileOperations.h"
#include "platform/Platform.h"
#include "SubtitleInfo.h"
#include "ImageReader.h"
#include "ShellIntegration.h"
#include "ScanCache.h"
#include <QApplication>
#include <array>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

using Language::text;

void MainWindow::chooseFolders() {
    if(scanning_ || fileOperationPending_ || closing_) return;
    ScanSourcesDialog dialog(folders_,options_,this);
    const int outcome=dialog.exec(); if(outcome!=QDialog::Accepted && outcome!=ScanSourcesDialog::SaveOnly) return;
    if(!dialog.applyTo(folders_,options_,this)) return;
    updateSourceSummary(); saveSettings(); updateEmptyState();
    if(outcome==QDialog::Accepted) { startScan(); return; }
    // Saving without a rescan must still protect the files already on screen, exactly as a scan would mark them.
    bool changed=false;
    for(int index=0;index<int(files_.size());++index) {
        auto& file=files_[index];
        const bool inFolder=std::any_of(options_.protectedFolders.begin(),options_.protectedFolders.end(),[&](const auto& root) { return VideoScanner::IsWithinFolder(file.fullPath,root); });
        if(inFolder==file.protectedByFolder) continue;
        file.protectedFile=file.protectedByFolder=inFolder; changed=true;
        if(inFolder) markedFiles_.remove(index);
    }
    if(!changed) return;
    reviewDirty_=true;
    for(auto panel:{left_,right_}) if(panel->fileIndex>=0) panel->setFile(&files_[panel->fileIndex],panel->fileIndex);
    for(int group=0;group<int(groups_.size());++group) updateGroupRow(group);
    applyReferenceRule(); updateReviewState();
}

// Every change applies at once, like the menu toggles these replace, so the window has only Close.
void MainWindow::showSettings() {
    QDialog dialog(this); dialog.setObjectName("settingsDialog"); dialog.setMinimumWidth(560);
    auto layout=new QVBoxLayout(&dialog); layout->setContentsMargins(22,22,22,18); layout->setSpacing(14);
    auto note=[](QWidget* parent) { auto label=new QLabel(parent); label->setObjectName("settingsNote"); label->setWordWrap(true); return label; };

    auto appearance=new QGroupBox(&dialog); auto appearanceForm=new QFormLayout(appearance); appearanceForm->setSpacing(10); appearanceForm->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint); appearanceForm->setLabelAlignment(Qt::AlignLeft|Qt::AlignVCenter);
    auto language=new QComboBox(appearance); language->setObjectName("settingsLanguage"); language->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    auto languages=Language::available();
    std::sort(languages.begin(),languages.end(),[](const auto& a,const auto& b) { return QString::localeAwareCompare(Language::displayName(a),Language::displayName(b))<0; });
    language->addItem(QString(),Language::systemCode);
    for(const auto& info:languages) language->addItem(Language::displayName(info),info.code);
    language->setCurrentIndex(std::max(0,language->findData(Language::selected())));
    auto theme=new QComboBox(appearance); theme->setObjectName("settingsTheme"); theme->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    for(const auto& info:Theme::themes()) theme->addItem(QString(),QString(info.id));
    theme->setCurrentIndex(std::max(0,theme->findData(Theme::current())));
    auto languageLabel=new QLabel(appearance), themeLabel=new QLabel(appearance);
    appearanceForm->addRow(languageLabel,language); appearanceForm->addRow(themeLabel,theme);
    // Translators work on files, so the folder they go in is one click away.
    auto languageNote=note(appearance); languageNote->setTextInteractionFlags(Qt::LinksAccessibleByMouse|Qt::LinksAccessibleByKeyboard);
    connect(languageNote,&QLabel::linkActivated,this,[] {
        // The folder may be new (a per-user one), so it gets the template the note tells translators to copy.
        const auto folder=Language::userFolder(); QDir().mkpath(folder);
        const auto templatePath=QDir(folder).filePath("template.json");
        // Copies of resources are read-only; the translator edits theirs.
        if(!QFileInfo::exists(templatePath) && QFile::copy(":/languages/template.json",templatePath))
            QFile::setPermissions(templatePath,QFile::ReadOwner|QFile::WriteOwner|QFile::ReadGroup|QFile::ReadOther);
        QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
    });
    appearanceForm->addRow(languageNote);
    layout->addWidget(appearance);

    auto windows=new QGroupBox(&dialog); auto windowsLayout=new QVBoxLayout(windows); windowsLayout->setSpacing(6);
    auto sendTo=new QCheckBox(windows); sendTo->setObjectName("settingsSendTo"); sendTo->setChecked(ShellIntegration::owned(ShellIntegration::shortcutPath()));
    auto sendToNote=note(windows); sendToNote->setContentsMargins(21,0,0,6);
    auto gpu=new QCheckBox(windows); gpu->setObjectName("settingsGpu");
    auto gpuNote=note(windows); gpuNote->setContentsMargins(21,0,0,0);
    for(QWidget* widget:{static_cast<QWidget*>(sendTo),static_cast<QWidget*>(sendToNote),static_cast<QWidget*>(gpu),static_cast<QWidget*>(gpuNote)}) windowsLayout->addWidget(widget);
    connect(sendTo,&QCheckBox::clicked,&dialog,[this,sendTo,&dialog](bool checked) {
        const auto path=ShellIntegration::shortcutPath();
        const bool done=checked?ShellIntegration::install(path,QCoreApplication::applicationFilePath()):ShellIntegration::uninstall(path);
        { QSignalBlocker blocker(sendTo); sendTo->setChecked(ShellIntegration::owned(path)); }
        if(!done) QMessageBox::warning(&dialog,text("Send To"),text(checked?"The Send To shortcut could not be saved. An unrelated existing shortcut is kept.":"No Compareel shortcut was removed."));
    });
    // Explorer's Send To menu exists only on Windows; elsewhere the box holds the playback option alone.
    const bool sendToAvailable=!ShellIntegration::shortcutPath().isEmpty();
    sendTo->setVisible(sendToAvailable); sendToNote->setVisible(sendToAvailable);
    layout->addWidget(windows);

    auto dataGroup=new QGroupBox(&dialog); auto dataLayout=new QGridLayout(dataGroup); dataLayout->setHorizontalSpacing(12); dataLayout->setVerticalSpacing(8); dataLayout->setColumnStretch(0,1);
    auto remember=new QCheckBox(dataGroup); remember->setObjectName("settingsRememberReview");
    auto forget=new QPushButton(dataGroup); forget->setObjectName("settingsForgetReview");
    auto diskCache=new QCheckBox(dataGroup); diskCache->setObjectName("settingsDiskCache");
    auto clearCache=new QPushButton(dataGroup); clearCache->setObjectName("settingsClearCache");
    const auto dataFolder=QFileInfo(settings_.fileName()).absolutePath();
    auto folderLabel=note(dataGroup); folderLabel->setWordWrap(false); folderLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto openFolder=new QPushButton(dataGroup); openFolder->setObjectName("settingsOpenDataFolder");
    auto privacy=new QPushButton(dataGroup); privacy->setObjectName("settingsDataPrivacy");
    // Only Close may be the default button; otherwise a disabled Forget is drawn with the focus border.
    for(auto button:{forget,clearCache,openFolder,privacy}) button->setAutoDefault(false);
    dataLayout->addWidget(remember,0,0); dataLayout->addWidget(forget,0,1);
    dataLayout->addWidget(diskCache,1,0); dataLayout->addWidget(clearCache,1,1);
    auto folderRow=new QHBoxLayout; folderRow->setSpacing(8); folderRow->addWidget(folderLabel,1); folderRow->addWidget(openFolder); folderRow->addWidget(privacy);
    // Not shown in a Flatpak, which Flathub updates.
    auto updates=new QCheckBox(dataGroup); updates->setObjectName("settingsUpdateCheck"); updates->setChecked(settings_.value("updates/check",false).toBool());
    updates->setVisible(updateChecksAvailable()); dataLayout->addWidget(updates,2,0,1,2);
    connect(updates,&QCheckBox::toggled,this,[this](bool on) { settings_.setValue("updates/check",on); settings_.sync(); });
    dataLayout->addLayout(folderRow,3,0,1,2);
    connect(forget,&QPushButton::clicked,forgetReviewAction_,&QAction::trigger);
    connect(clearCache,&QPushButton::clicked,findChild<QAction*>("Clear scan cache"),&QAction::trigger);
    connect(openFolder,&QPushButton::clicked,this,[dataFolder] { QDesktopServices::openUrl(QUrl::fromLocalFile(dataFolder)); });
    connect(privacy,&QPushButton::clicked,findChild<QAction*>("Data and privacy"),&QAction::trigger);
    auto clearHistory=new QPushButton(dataGroup); clearHistory->setObjectName("settingsClearHistory"); clearHistory->setAutoDefault(false); folderRow->addWidget(clearHistory);
    connect(clearHistory,&QPushButton::clicked,findChild<QAction*>("Clear history…"),&QAction::trigger);
    layout->addWidget(dataGroup);

    // Checkboxes drive the existing actions, so guards, persistence and status messages stay in one place.
    auto bind=[](QCheckBox* box,QAction* item) {
        box->setChecked(item->isChecked());
        connect(box,&QCheckBox::clicked,item,[item](bool checked) { if(item->isChecked()!=checked) item->trigger(); });
        connect(item,&QAction::toggled,box,[box](bool checked) { QSignalBlocker blocker(box); box->setChecked(checked); });
    };
    bind(gpu,findChild<QAction*>("Prefer GPU video decoding (experimental)")); bind(remember,rememberReviewAction_); bind(diskCache,findChild<QAction*>("Store media cache on disk"));
    auto refreshForget=[this,forget] { forget->setEnabled(QFileInfo::exists(lastReviewPath_)); };
    connect(rememberReviewAction_,&QAction::toggled,forget,refreshForget); refreshForget();

    auto buttons=new QDialogButtonBox(QDialogButtonBox::Close,&dialog); layout->addWidget(buttons);
    connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);

    auto retranslate=[&] {
        dialog.setWindowTitle(text("Settings"));
        appearance->setTitle(text("Appearance")); languageLabel->setText(text("Language")); themeLabel->setText(text("Theme"));
        language->setItemText(0,text("Use system language"));
        for(int i=0;i<theme->count();++i) theme->setItemText(i,text(Theme::themes()[size_t(i)].name));
        languageNote->setText(text("To add a language, copy template.json in the %1 and translate it.").arg("<a href=\"languages\" style=\"color:"+Theme::color("@link").name()+"\">"+text("languages folder").toHtmlEscaped()+"</a>"));
        windows->setTitle(text(sendToAvailable?"Windows":"Playback")); sendTo->setText(text("Show Compareel in Explorer's Send To menu"));
        sendToNote->setText(text("Select two files in Explorer and choose Send to > Compareel."));
        gpu->setText(text("Prefer GPU video decoding (experimental)"));
        gpuNote->setText(text("Decoder preference applies to newly opened videos and uncached scans; unsupported GPU decoding falls back to CPU."));
        dataGroup->setTitle(text("Data")); remember->setText(text("Remember last review")); forget->setText(text("Forget last review"));
        diskCache->setText(text("Store media cache on disk")); diskCache->setToolTip(findChild<QAction*>("Store media cache on disk")->toolTip());
        clearCache->setText(text("Clear scan cache")); updates->setText(text("Check for updates when Compareel starts"));
        folderLabel->setText(text("Local data folder")+": "+fontMetrics().elidedText(QDir::toNativeSeparators(dataFolder),Qt::ElideMiddle,260)); folderLabel->setToolTip(QDir::toNativeSeparators(dataFolder));
        openFolder->setText(text("Open folder")); privacy->setText(text("Data and privacy")); clearHistory->setText(text("Clear history…"));
        buttons->button(QDialogButtonBox::Close)->setText(text("Close"));
        // Equal widths keep the two choices aligned as one column, whatever the longest name in this language.
        const int width=std::max({200,language->sizeHint().width(),theme->sizeHint().width()});
        for(auto combo:{language,theme}) combo->setMinimumWidth(width);
    };
    retranslate();
    connect(language,&QComboBox::activated,&dialog,[&,this] {
        Language::setCurrent(language->currentData().toString()); translate(); populateResults(); saveSettings(); retranslate();
    });
    connect(theme,&QComboBox::activated,&dialog,[&,this] { setTheme(theme->currentData().toString()); retranslate(); });
    dialog.exec();
}

void MainWindow::advancedSettings() {
    if(scanning_ || fileOperationPending_ || closing_) return;
    ScanRulesDialog dialog(options_,profile_->currentIndex()==2,this);
    if(dialog.exec()!=QDialog::Accepted) return;
    if(!dialog.applyTo(options_,this)) return;
    if(similarityThreshold_) { QSignalBlocker b(similarityThreshold_); similarityThreshold_->setValue(static_cast<int>(std::lround(options_.minFrameSimilarity))); }
    updateMediaMode(); saveSettings();
}

void MainWindow::showClearHistory() {
    if(reviewSaving_ || scanning_ || sortingMetadata_ || clearingCache_ || fileOperationPending_ || closing_) { statusBar()->showMessage(text("Wait for the current operation to finish.")); return; }
    // As a browser's "clear browsing data": short names for each kind of kept data, all ticked, details on hover.
    QDialog dialog(this); dialog.setObjectName("clearHistoryDialog"); dialog.setWindowTitle(text("Clear history…"));
    auto layout=new QVBoxLayout(&dialog); layout->setContentsMargins(20,20,20,20); layout->setSpacing(10);
    auto lead=new QLabel(text("Only Compareel's own records are removed."),&dialog); lead->setWordWrap(true); layout->addWidget(lead);
    struct Item { const char* label; const char* detail; QCheckBox* box=nullptr; };
    std::array<Item,5> items{{{"Scan results","The results on screen and the last scan report."},
        {"Reviews and decisions","The saved last review, the cleanup recovery review, review decisions and ignored pairs."},
        {"File operation history","Copies, moves and removals made by Compareel. They can no longer be undone afterwards."},
        {"Scan cache","Thumbnails and signatures kept to make later scans faster."},
        {"Recent folders","The included, excluded and protected source folders."}}};
    for(auto& item:items) { item.box=new QCheckBox(text(item.label),&dialog); item.box->setToolTip(text(item.detail)); item.box->setChecked(true); layout->addWidget(item.box); }
    auto buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(text("Clear history")); buttons->button(QDialogButtonBox::Cancel)->setText(text("Cancel"));
    buttons->button(QDialogButtonBox::Ok)->setAutoDefault(false); buttons->button(QDialogButtonBox::Cancel)->setDefault(true);
    connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept); connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject); layout->addWidget(buttons);
    if(dialog.exec()!=QDialog::Accepted) return;
    const auto dataFolder=QFileInfo(settings_.fileName()).dir();
    QStringList failures; auto remove=[&](const QString& path) { if(QFileInfo::exists(path) && !QFile::remove(path)) failures<<QDir::toNativeSeparators(path); };
    if(items[0].box->isChecked()) {
        clearResults(); remove(dataFolder.filePath("last-scan-report.txt"));
        scanMetrics_={}; lastScanOutcome_.clear(); lastScanElapsedMs_=0; settings_.remove("lastScanOutcome"); settings_.remove("lastScanElapsedMs"); updateScanTimeLabel();
    }
    previewPool_.waitForDone();
    if(items[1].box->isChecked()) {
        remove(lastReviewPath_); remove(cleanupReviewPath_); settings_.remove("cleanupInProgress"); resumeReviewAction_->setEnabled(QFileInfo::exists(lastReviewPath_));
    }
    {
        ScanCache cache(cachePath_);
        if(items[1].box->isChecked()) { cache.clearReviewDecisions(); cache.clearIgnoredPairs(); }
        if(items[2].box->isChecked()) cache.clearOperationHistory();
        if(items[3].box->isChecked()) {
            cache.clearScanData(); remove(cachePath_+".scan-job"); pausedScan_=false; resumeAction_->setEnabled(false); settings_.remove("scanInProgress");
        }
        if(!cache.error().isEmpty()) failures<<cache.error();
    }
    if(items[1].box->isChecked()) refreshReviewDecisions();
    if(items[2].box->isChecked()) updateUndoAction();
    if(items[4].box->isChecked()) {
        folders_.clear(); options_.excludedFolders.clear(); options_.protectedFolders.clear(); options_.nonRecursiveFolders.clear(); updateSourceSummary();
    }
    saveSettings(); settings_.sync(); updateEmptyState();
    if(failures.isEmpty()) statusBar()->showMessage(text("History cleared."));
    else QMessageBox::warning(this,text("Clear history…"),text("Some items could not be removed:")+"\n\n"+failures.join("\n"));
}

void MainWindow::showFileDetails(PlayerPanel* panel) {
    if(panel->fileIndex<0) return; auto file=files_[panel->fileIndex];
    QDialog dialog(this); dialog.setObjectName("fileDetailsDialog"); dialog.setWindowTitle(text("File details"));
    auto cancelled=std::make_shared<std::atomic<bool>>(false);
    connect(&dialog,&QDialog::finished,&dialog,[cancelled] { cancelled->store(true); });
    auto layout=new QVBoxLayout(&dialog); auto form=new QFormLayout; form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow); layout->addLayout(form);
    // Labels sit on the first line of values that wrap, such as long folders, with room before the values.
    form->setLabelAlignment(Qt::AlignLeft|Qt::AlignTop); form->setHorizontalSpacing(16);
    auto row=[&](const char* label,QString value,const char* id="") {
        auto field=new QLabel(value,&dialog); field->setObjectName(QString::fromUtf8(id)); field->setTextFormat(Qt::PlainText); field->setTextInteractionFlags(Qt::TextSelectableByMouse); field->setWordWrap(true); field->setMinimumWidth(320); field->setMaximumWidth(520);
        // The form gives a label a cell somewhat taller than one line and QLabel centres text in it, so the text itself is top-aligned.
        auto name=new QLabel(text(label),&dialog); name->setAlignment(Qt::AlignLeft|Qt::AlignTop); form->addRow(name,field); return field;
    };
    row("File name",QString::fromStdWString(file.fileName)); row("Folder",QDir::toNativeSeparators(QString::fromStdWString(file.folderPath)),"detailFolder");
    row("Size",QString::fromStdWString(FormatBytes(file.fileSize)));
    const bool image=StillImages::supported(file.fullPath);
    auto durationField=row("Duration",{},"detailDuration"),resolutionField=row("Resolution",{},"detailResolution"),codecField=row(image?"Image format":"Video codec",{});
    auto fpsField=row("Frame rate",{},"detailFps"),bitrateField=row("Video bitrate",{}),audioField=row("Audio",{});
    if(image) for(auto field:{durationField,fpsField,bitrateField,audioField}) form->setRowVisible(field,false);
    auto refreshMetadata=[=](const VideoFileInfo& info) {
        const auto unknown=text(info.metadataLoaded?"Not available":"Inspecting…");
        durationField->setText(info.duration100ns>0?duration(info.duration100ns):unknown);
        resolutionField->setText(info.width && info.height?QString("%1 × %2").arg(info.width).arg(info.height):unknown);
        codecField->setText(info.codec.empty()?unknown:QString::fromStdWString(info.codec));
        fpsField->setText(info.fps>0?QString("%1 FPS").arg(info.fps,0,'f',3):unknown);
        bitrateField->setText(info.videoBitrate?QString("%1 kb/s").arg(info.videoBitrate/1000.0,0,'f',1):unknown);
        audioField->setText(info.audioChannels<0?unknown:info.audioChannels==0?text("No audio track"):QString("%1 · %2 ch · %3 Hz").arg(QString::fromStdWString(info.audioCodec)).arg(info.audioChannels).arg(info.audioSampleRate));
    };
    refreshMetadata(file);
    if(!file.metadataLoaded) {
        auto watcher=new QFutureWatcher<VideoFileInfo>(&dialog);
        connect(watcher,&QFutureWatcher<VideoFileInfo>::finished,&dialog,[watcher,refreshMetadata] { refreshMetadata(watcher->result()); watcher->deleteLater(); });
        watcher->setFuture(QtConcurrent::run([file,cancelled] {
            VideoFileInfo result; result.fullPath=file.fullPath; result.identity=file.identity; result.metadataLoaded=true;
            Platform::ComInit com(Platform::ComApartment::MultiThreaded);
            if(cancelled->load()) result.error=E_ABORT;
            else if(!com.succeeded()) result.error=com.hr();
            else if(!file.identity.valid || FileOperations::ReadIdentity(file.fullPath)!=file.identity) result.error=HRESULT_FROM_WIN32(ERROR_FILE_INVALID);
            else {
                result.metadataLoaded=false;
                if(StillImages::supported(result.fullPath)) StillImages::analyze(result,*cancelled); else VideoScanner::ExtractMetadataOnly(result);
                if(FileOperations::ReadIdentity(file.fullPath)!=file.identity) { result={}; result.metadataLoaded=true; result.error=HRESULT_FROM_WIN32(ERROR_FILE_INVALID); }
            }
            return result;
        }));
    }
    auto subtitles=row("Subtitles",text("Not inspected"),"detailSubtitles");
    if(image) form->setRowVisible(subtitles,false);
    if(!image) {
        subtitles->setText(text("Inspecting…"));
        auto watcher=new QFutureWatcher<SubtitleInfo>(&dialog);
        connect(watcher,&QFutureWatcher<SubtitleInfo>::finished,&dialog,[watcher,subtitles] {
            const auto result=watcher->result(); watcher->deleteLater();
            if(result.status!=SubtitleInfo::Complete) {
                subtitles->setText(result.status==SubtitleInfo::Changed?text("File changed; inspect again"):text("Not inspected"));
                subtitles->setToolTip(text("Media inspection did not finish successfully.")); return;
            }
            auto summary=result.tracks.empty()?text("No embedded subtitles reported"):text("%1 embedded track(s)").arg(result.tracks.size());
            if(result.tracks.size()==1) summary+=" · "+result.tracks.front();
            subtitles->setText(summary);
            if(!result.tracks.empty()) subtitles->setToolTip("<pre>"+(result.tracks.mid(0,8).join('\n')+(result.tracks.size()>8?"\n…":QString())).toHtmlEscaped()+"</pre>");
        });
        watcher->setFuture(QtConcurrent::run([path=QString::fromStdWString(file.fullPath),identity=file.identity,cancelled] { return inspectSubtitles(path,identity,*cancelled); }));
    }
    row("Modified",file.identity.valid?QDateTime::fromMSecsSinceEpoch(qint64(file.identity.modified/10000)-11644473600000LL).toLocalTime().toString("yyyy-MM-dd HH:mm:ss"):text("Not available"));
    row("Quality",text("File size, bitrate and resolution alone do not determine quality."));
    auto close=new QDialogButtonBox(QDialogButtonBox::Close,&dialog); close->button(QDialogButtonBox::Close)->setText(text("Close")); connect(close,&QDialogButtonBox::rejected,&dialog,&QDialog::reject); layout->addWidget(close); dialog.exec();
}

void MainWindow::showOperationHistory() {
    OperationHistoryDialog dialog(cachePath_,[this] { updateUndoAction(); },this);
    dialog.exec();
}
