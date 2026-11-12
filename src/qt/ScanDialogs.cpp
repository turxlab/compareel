#include "ScanDialogs.h"
#include "MainWindowInternal.h"
#include "Language.h"
#include "Icons.h"
#include "Theme.h"
#include "ScanCache.h"
#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QShortcut>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

using Language::text;

void ScanSourcesDialog::addIncluded(const QString& rawPath, const ScanCriteria& options) {
    const QString path=QDir::toNativeSeparators(QDir::cleanPath(rawPath));
    for(int i=0;i<included_->topLevelItemCount();++i)
        if(samePath(included_->topLevelItem(i)->text(0),path)) return;
    auto item=new QTreeWidgetItem(included_,{path,{},{}});
    const bool isFile=QFileInfo(path).isFile();
    const bool nonRec=std::any_of(options.nonRecursiveFolders.begin(),options.nonRecursiveFolders.end(),[&](const auto& nr) {
        return samePath(QString::fromStdWString(nr),path);
    });
    const bool protectedPath=std::any_of(options.protectedFolders.begin(),options.protectedFolders.end(),[&](const auto& pr) {
        return samePath(QString::fromStdWString(pr),path);
    });
    item->setData(0,Qt::UserRole,isFile);
    if(!isFile) item->setCheckState(1,(options.includeSubfolders && !nonRec)?Qt::Checked:Qt::Unchecked);
    else item->setText(1,"—");
    item->setCheckState(2,protectedPath?Qt::Checked:Qt::Unchecked);
    item->setToolTip(0,path);
}

ScanSourcesDialog::ScanSourcesDialog(const std::vector<std::wstring>& folders, const ScanCriteria& options, QWidget* parent)
    : QDialog(parent) {
    setObjectName("scanSourcesDialog"); setWindowTitle(text("Scan sources")); resize(760,560);
    auto layout=new QVBoxLayout(this); layout->setContentsMargins(20,20,20,20); layout->setSpacing(12);
    included_=new QTreeWidget(this); included_->setObjectName("includedSourcesTree");
    included_->setColumnCount(3); included_->setHeaderLabels({text("Folder or file"),text("Subfolders"),text("Protected")});
    included_->header()->setSectionResizeMode(0,QHeaderView::Stretch);
    included_->header()->setSectionResizeMode(1,QHeaderView::ResizeToContents);
    included_->header()->setSectionResizeMode(2,QHeaderView::ResizeToContents);
    included_->setRootIsDecorated(false);
    for(const auto& path:strings(folders)) addIncluded(path,options);
    auto lead=new QLabel(text("Choose the folders and files to scan"),this); lead->setObjectName("dialogLead"); layout->addWidget(lead);
    auto includeBox=new QGroupBox(text("Included folders and files"),this); auto includeLayout=new QVBoxLayout(includeBox); includeLayout->setSpacing(8);
    includeLayout->addWidget(included_,1);
    auto buttons=new QHBoxLayout;
    auto add=new QPushButton(text("Add folder…"),this); add->setObjectName("addScanFolderButton"); add->setIcon(icon(Glyph::Folder));
    auto addFiles=new QPushButton(text("Add files…"),this); addFiles->setObjectName("addScanFilesButton"); addFiles->setIcon(icon(Glyph::Files));
    auto remove=new QPushButton(text("Remove"),this); remove->setObjectName("removeScanSourceButton"); remove->setEnabled(false);
    auto removeAll=new QPushButton(text("Remove all"),this); removeAll->setObjectName("removeAllScanSourcesButton");
    buttons->addWidget(add); buttons->addWidget(addFiles); buttons->addWidget(remove); buttons->addWidget(removeAll); buttons->addStretch();
    includeLayout->addLayout(buttons);
    // On its own line: next to four buttons it would push the dialog wider than the screen in longer languages.
    recursive_=new QCheckBox(text("Include subfolders for all folders"),this); recursive_->setObjectName("includeSubfoldersCheck"); recursive_->setChecked(options.includeSubfolders); includeLayout->addWidget(recursive_);
    auto note=new QLabel(text("Protected folders are scanned, but their files cannot be removed."),this); note->setObjectName("secondary"); note->setWordWrap(true); includeLayout->addWidget(note);
    layout->addWidget(includeBox,1);
    // New rows follow the dialog's current subfolder choice, not the one it opened with.
    auto currentOptions=[this,initialOptions=options] { auto value=initialOptions; value.includeSubfolders=recursive_->isChecked(); return value; };
    connect(add,&QPushButton::clicked,this,[this,currentOptions] { auto path=QFileDialog::getExistingDirectory(this,text("Add folder…")); if(!path.isEmpty()) addIncluded(path,currentOptions()); });
    connect(addFiles,&QPushButton::clicked,this,[this,currentOptions] {
        const auto paths=QFileDialog::getOpenFileNames(this,text("Add files…"),{},text("All files (*)"));
        for(const auto& path:paths) if(!path.isEmpty()) addIncluded(path,currentOptions());
    });
    // Several rows can be selected; Remove and the Delete key drop all of them. Nothing changes until the dialog is saved.
    included_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    auto updateRemoveButtons=[this,remove,removeAll] { remove->setEnabled(!included_->selectedItems().isEmpty()); removeAll->setEnabled(included_->topLevelItemCount()>0); };
    auto removeSelected=[this,updateRemoveButtons] { qDeleteAll(included_->selectedItems()); updateRemoveButtons(); };
    connect(included_,&QTreeWidget::itemSelectionChanged,this,updateRemoveButtons);
    connect(included_->model(),&QAbstractItemModel::rowsInserted,this,updateRemoveButtons);
    connect(remove,&QPushButton::clicked,this,removeSelected);
    connect(removeAll,&QPushButton::clicked,this,[this,updateRemoveButtons] { included_->clear(); updateRemoveButtons(); });
    for(const auto& keys:deleteShortcuts()) connect(new QShortcut(keys,included_,nullptr,nullptr,Qt::WidgetShortcut),&QShortcut::activated,this,removeSelected);
    updateRemoveButtons();
    connect(recursive_,&QCheckBox::toggled,this,[this](bool checked) {
        for(int i=0;i<included_->topLevelItemCount();++i) {
            auto item=included_->topLevelItem(i);
            if(!item->data(0,Qt::UserRole).toBool()) item->setCheckState(1,checked?Qt::Checked:Qt::Unchecked);
        }
    });
    // Excluded folders are a list with the same add/remove pattern, not a semicolon-separated text field.
    auto excludeBox=new QGroupBox(text("Excluded folders"),this); auto excludeLayout=new QHBoxLayout(excludeBox);
    excluded_=new QListWidget(this); excluded_->setObjectName("excludedFoldersList"); excluded_->setMaximumHeight(84);
    for(const auto& path:strings(options.excludedFolders)) excluded_->addItem(path);
    auto excludeButtons=new QVBoxLayout;
    auto addExcluded=new QPushButton(text("Add folder…"),this); addExcluded->setObjectName("addExcludedFolderButton"); addExcluded->setAccessibleName(text("Excluded folders"));
    auto removeExcluded=new QPushButton(text("Remove"),this); removeExcluded->setObjectName("removeExcludedFolderButton"); removeExcluded->setEnabled(false);
    excludeButtons->addWidget(addExcluded); excludeButtons->addWidget(removeExcluded); excludeButtons->addStretch();
    excludeLayout->addWidget(excluded_,1); excludeLayout->addLayout(excludeButtons); layout->addWidget(excludeBox);
    connect(addExcluded,&QPushButton::clicked,this,[this] {
        auto path=QFileDialog::getExistingDirectory(this,text("Excluded folders")); if(path.isEmpty()) return;
        path=QDir::toNativeSeparators(QDir::cleanPath(path)); if(excluded_->findItems(path,Qt::MatchFixedString).isEmpty()) excluded_->addItem(path);
    });
    excluded_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    connect(excluded_,&QListWidget::itemSelectionChanged,this,[this,removeExcluded] { removeExcluded->setEnabled(!excluded_->selectedItems().isEmpty()); });
    auto removeExcludedSelected=[this] { qDeleteAll(excluded_->selectedItems()); };
    connect(removeExcluded,&QPushButton::clicked,this,removeExcludedSelected);
    for(const auto& keys:deleteShortcuts()) connect(new QShortcut(keys,excluded_,nullptr,nullptr,Qt::WidgetShortcut),&QShortcut::activated,this,removeExcludedSelected);
    crossFolders_=new QCheckBox(text("Compare only between selected folders"),this); crossFolders_->setObjectName("crossFolderOnlyCheck"); crossFolders_->setChecked(options.crossFolderOnly); layout->addWidget(crossFolders_);
    crossFolders_->setToolTip(text("Subfolders belong to their selected root. Each candidate is compared with a file from another root; protection is a separate setting."));
    auto filtersHint=new QLabel(text("Size and extension filters are in Tools > Scan rules."),this); filtersHint->setObjectName("secondary"); filtersHint->setWordWrap(true); layout->addWidget(filtersHint);
    // Sources can be saved without scanning; Scan in the toolbar then reuses them.
    auto box=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,this); box->button(QDialogButtonBox::Ok)->setText(text("Save and scan")); box->button(QDialogButtonBox::Cancel)->setText(text("Cancel"));
    auto save=box->addButton(text("Save"),QDialogButtonBox::ActionRole); save->setObjectName("saveSourcesButton");
    connect(save,&QPushButton::clicked,this,[this] { done(SaveOnly); });
    connect(box,&QDialogButtonBox::accepted,this,&QDialog::accept); connect(box,&QDialogButtonBox::rejected,this,&QDialog::reject); layout->addWidget(box);
}

bool ScanSourcesDialog::applyTo(std::vector<std::wstring>& folders, ScanCriteria& options, QWidget* messageParent) const {
    if(crossFolders_->isChecked() && included_->topLevelItemCount()<2) { QMessageBox::information(messageParent,text("Scan sources"),text("Select at least two different folders for cross-folder comparison.")); return false; }
    options.crossFolderOnly=crossFolders_->isChecked();
    folders.clear(); options.protectedFolders.clear(); options.nonRecursiveFolders.clear();
    bool anyFolderChecked=false,anyFolder=false;
    for(int i=0;i<included_->topLevelItemCount();++i) {
        auto item=included_->topLevelItem(i);
        auto path=item->text(0).toStdWString();
        folders.push_back(path);
        if(item->checkState(2)==Qt::Checked) options.protectedFolders.push_back(path);
        if(!item->data(0,Qt::UserRole).toBool()) {
            anyFolder=true;
            if(item->checkState(1)==Qt::Checked) anyFolderChecked=true;
            else options.nonRecursiveFolders.push_back(path);
        }
    }
    options.includeSubfolders=anyFolder?anyFolderChecked:recursive_->isChecked();
    if(!options.includeSubfolders) options.nonRecursiveFolders.clear();
    QStringList excludedPaths; for(int i=0;i<excluded_->count();++i) excludedPaths<<excluded_->item(i)->text();
    options.excludedFolders=paths(excludedPaths);
    return true;
}

ScanRulesDialog::ScanRulesDialog(const ScanCriteria& options, int activeProfile, QWidget* parent)
    : QDialog(parent), initialMinBytes_(options.minFileSize), initialMaxBytes_(options.maxFileSize) {
    setObjectName("advancedSettingsDialog"); setWindowTitle(text("Scan rules…")); resize(620,0);
    const bool customProfileActive = (activeProfile == 2);
    auto layout=new QVBoxLayout(this); layout->setContentsMargins(22,22,22,22); layout->setSpacing(12);
    auto form=new QFormLayout; form->setSpacing(10); auto filters=new QFormLayout; filters->setSpacing(10); auto performance=new QFormLayout; performance->setSpacing(10);
    auto initSizeControl=[this](uint64_t bytes, bool isMax, QDoubleSpinBox*& spin, QComboBox*& unit) -> QWidget* {
        int unitIdx = 1; // MB default
        double value = 0.0;
        if(isMax && bytes==UINT64_MAX) { unitIdx=1; value=0.0; }
        else if(!isMax && bytes<=1) { unitIdx=1; value=0.0; }
        else if(bytes>=1024ULL*1024ULL*1024ULL && (bytes%(1024ULL*1024ULL*1024ULL)==0)) { unitIdx=2; value=double(bytes)/(1024.0*1024.0*1024.0); }
        else if(bytes>=1024ULL*1024ULL) { unitIdx=1; value=double(bytes)/(1024.0*1024.0); }
        else { unitIdx=0; value=double(bytes)/1024.0; }
        auto row=new QWidget(this); auto rowLayout=new QHBoxLayout(row); rowLayout->setContentsMargins(0,0,0,0); rowLayout->setSpacing(6);
        spin=new QDoubleSpinBox(row); spin->setRange(0,100000000); spin->setDecimals(2); spin->setValue(value);
        unit=new QComboBox(row); unit->addItems({"KB","MB","GB"}); unit->setCurrentIndex(unitIdx);
        rowLayout->addWidget(spin,1); rowLayout->addWidget(unit);
        return row;
    };
    auto minRow=initSizeControl(options.minFileSize,false,minimum_,minUnit_); minimum_->setObjectName("minimumSizeSpin"); minUnit_->setObjectName("minimumSizeUnit");
    auto maxRow=initSizeControl(options.maxFileSize,true,maximum_,maxUnit_); maximum_->setObjectName("maximumSizeSpin"); maxUnit_->setObjectName("maximumSizeUnit");
    connect(minimum_,QOverload<double>::of(&QDoubleSpinBox::valueChanged),this,[this] { minEdited_=true; });
    connect(minUnit_,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this] { minEdited_=true; });
    connect(maximum_,QOverload<double>::of(&QDoubleSpinBox::valueChanged),this,[this] { maxEdited_=true; });
    connect(maxUnit_,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this] { maxEdited_=true; });
    extensions_=new QLineEdit(this); extensions_->setObjectName("includedExtensionsEdit"); QStringList ext; for(auto value:options.extensions) ext<<QString::fromStdWString(value); extensions_->setText(ext.join(", ")); extensions_->setPlaceholderText("mp4, mkv, mov, webm");
    excludedExtensions_=new QLineEdit(this); excludedExtensions_->setObjectName("advancedExcludedExtensionsEdit"); QStringList exclExt; for(auto value:options.excludedExtensions) exclExt<<QString::fromStdWString(value); excludedExtensions_->setText(exclExt.join(", ")); excludedExtensions_->setPlaceholderText(text("None (for example: gif, webp, svg)"));
    workers_=new QSpinBox(this); workers_->setRange(1,16); workers_->setValue(options.workerCount);
    threshold_=new QDoubleSpinBox(this); threshold_->setObjectName("advancedSimilarityThreshold"); threshold_->setRange(70,100); threshold_->setSuffix(" %"); threshold_->setValue(options.minFrameSimilarity);
    tolerance_=new QDoubleSpinBox(this); tolerance_->setRange(0,60); tolerance_->setDecimals(2); tolerance_->setValue(options.durationTol100ns/10000000.0);
    // Sections: how files are matched, the custom criteria, which files are read, and how fast.
    filters->addRow(text("Minimum size"),minRow); filters->addRow(text("Maximum size (0 = unlimited)"),maxRow);
    filters->addRow(text(options.images?"Image extensions":"Video extensions"),extensions_);
    filters->addRow(text("Excluded extensions"),excludedExtensions_);
    performance->addRow(text("Parallel reads"),workers_);
    form->addRow(text("Similarity threshold"),threshold_); form->addRow(text("Duration tolerance (seconds)"),tolerance_);
    auto matching=new QGroupBox(text("Matching"),this); auto matchingLayout=new QVBoxLayout(matching); matchingLayout->addLayout(form); layout->addWidget(matching);
    transforms_=new QCheckBox(text("Recognize black borders, mirrored and rotated copies"),this); transforms_->setChecked(options.matchTransforms); matchingLayout->addWidget(transforms_);
    if(activeProfile==0) {
        threshold_->setEnabled(false); tolerance_->setEnabled(false); transforms_->setEnabled(false);
        auto exactNote=new QLabel(text("Exact copies compares full file contents; visual matching rules are not used in this mode."),matching);
        exactNote->setObjectName("secondary"); exactNote->setWordWrap(true); matchingLayout->addWidget(exactNote);
    }
    size_=new QCheckBox(text("Same file size"),this); size_->setChecked(options.matchSize);
    durationCheck_=new QCheckBox(text("Same duration"),this); durationCheck_->setChecked(options.matchDuration);
    if(options.images) { durationCheck_->setChecked(false); durationCheck_->hide(); form->setRowVisible(tolerance_,false); extensions_->setPlaceholderText("jpg, jpeg, png, webp, heic, heif, avif, bmp, gif, tif, tga, ico, svg, psd, jxl, jp2, exr, hdr, dds, pcx, dng, cr2, nef, arw"); }
    resolution_=new QCheckBox(text("Same resolution"),this); resolution_->setChecked(options.matchResolution);
    first_=new QCheckBox(text(options.images?"Similar image content":"Similar first frame"),this); first_->setChecked(options.matchFirstFrame);
    fileNameCheck_=new QCheckBox(text("Same file name"),this); fileNameCheck_->setObjectName("matchFileNameCheck"); fileNameCheck_->setChecked(options.matchFileName);
    extensionCheck_=new QCheckBox(text("Same file extension"),this); extensionCheck_->setObjectName("matchExtensionCheck"); extensionCheck_->setChecked(options.matchExtension);
    auto custom=new QGroupBox(text("Advanced criteria"),this); custom->setObjectName("customCriteriaGroup"); auto customLayout=new QGridLayout(custom);
    int cell=0; for(auto box:{size_,durationCheck_,resolution_,first_,fileNameCheck_,extensionCheck_}) { box->setEnabled(customProfileActive); customLayout->addWidget(box,cell/2,cell%2); ++cell; }
    if(customProfileActive) {
        auto syncMatching=[this,isImages=options.images] {
            const bool visual=first_->isChecked();
            threshold_->setEnabled(visual);
            transforms_->setEnabled(visual);
            tolerance_->setEnabled(!isImages && durationCheck_->isChecked());
        };
        connect(first_,&QCheckBox::toggled,this,syncMatching);
        connect(durationCheck_,&QCheckBox::toggled,this,syncMatching);
        syncMatching();
    }
    layout->addWidget(custom);
    auto note=new QLabel(text("Advanced criteria find candidates; they do not prove identical content."),this); note->setWordWrap(true); note->setObjectName("secondary"); customLayout->addWidget(note,3,0,1,2);
    if(!customProfileActive) { auto inactive=new QLabel(text("These rules apply when the comparison mode is Advanced criteria."),custom); inactive->setObjectName("secondary"); inactive->setWordWrap(true); customLayout->addWidget(inactive,4,0,1,2); }
    auto filterBox=new QGroupBox(text("File filters"),this); filterBox->setLayout(filters); layout->addWidget(filterBox);
    auto performanceBox=new QGroupBox(text("Performance"),this); performanceBox->setLayout(performance); layout->addWidget(performanceBox);
    auto box=new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel,this); box->button(QDialogButtonBox::Save)->setText(text("Save")); box->button(QDialogButtonBox::Cancel)->setText(text("Cancel"));
    connect(box,&QDialogButtonBox::accepted,this,&QDialog::accept); connect(box,&QDialogButtonBox::rejected,this,&QDialog::reject); layout->addWidget(box);
}

bool ScanRulesDialog::applyTo(ScanCriteria& options, QWidget* messageParent) const {
    auto unitBytes=[](int idx) -> uint64_t { return idx==0?1024ULL:idx==2?(1024ULL*1024ULL*1024ULL):(1024ULL*1024ULL); };
    const uint64_t minBytes=minEdited_
        ? std::max<uint64_t>(1,static_cast<uint64_t>(std::llround(minimum_->value()*double(unitBytes(minUnit_->currentIndex())))))
        : initialMinBytes_;
    const uint64_t maxBytes=maxEdited_
        ? (maximum_->value()<=0.0?UINT64_MAX:std::max<uint64_t>(1,static_cast<uint64_t>(std::llround(maximum_->value()*double(unitBytes(maxUnit_->currentIndex()))))))
        : initialMaxBytes_;
    if(maxBytes!=UINT64_MAX && minBytes>maxBytes) { QMessageBox::information(messageParent,text("Scan rules…"),text("Minimum size cannot exceed maximum size.")); return false; }
    options.minFileSize=minBytes;
    options.maxFileSize=maxBytes;
    options.extensions.clear(); for(auto extension:extensions_->text().split(',',Qt::SkipEmptyParts)) { extension=extension.trimmed().toLower(); if(!extension.startsWith('.')) extension.prepend('.'); options.extensions.push_back(extension.toStdWString()); }
    options.excludedExtensions.clear(); for(auto extension:excludedExtensions_->text().split(',',Qt::SkipEmptyParts)) { extension=extension.trimmed().toLower(); if(!extension.startsWith('.')) extension.prepend('.'); if(extension.size()>1) options.excludedExtensions.push_back(extension.toStdWString()); }
    options.matchTransforms=transforms_->isChecked();
    options.workerCount=workers_->value(); options.minFrameSimilarity=threshold_->value(); options.durationTol100ns=static_cast<int64_t>(tolerance_->value()*10000000);
    options.matchSize=size_->isChecked(); options.matchDuration=durationCheck_->isChecked(); options.matchResolution=resolution_->isChecked(); options.matchFirstFrame=first_->isChecked();
    options.matchFileName=fileNameCheck_->isChecked(); options.matchExtension=extensionCheck_->isChecked();
    if(!options.matchSize&&!options.matchDuration&&!options.matchResolution&&!options.matchFirstFrame&&!options.matchFileName&&!options.matchExtension) options.matchSize=true;
    return true;
}

OperationHistoryDialog::OperationHistoryDialog(const QString& cachePath, std::function<void()> onHistoryCleared, QWidget* parent)
    : QDialog(parent) {
    setObjectName("operationHistoryDialog"); setWindowTitle(text("Operation history")); resize(980,460);
    auto layout=new QVBoxLayout(this);
    auto table=new QTreeWidget(this); table->setObjectName("operationHistoryTable"); table->setRootIsDecorated(false); table->setUniformRowHeights(true); table->setTextElideMode(Qt::ElideMiddle);
    table->setHeaderLabels({text("Time"),text("Operation"),text("Result"),text("File"),text("Destination")}); table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table->header()->setStretchLastSection(false); table->header()->setSectionResizeMode(3,QHeaderView::Stretch); table->header()->setSectionResizeMode(4,QHeaderView::Stretch);
    auto empty=new QLabel(this); empty->setObjectName("secondary"); empty->setAlignment(Qt::AlignCenter); empty->setWordWrap(true);
    layout->addWidget(table,1); layout->addWidget(empty,1);
    auto cache=std::make_shared<ScanCache>(cachePath);
    auto entries=std::make_shared<std::vector<OperationEntry>>();
    auto fill=[this,table,empty,cache,entries] {
        *entries=cache->operationEntries(); table->clear(); bool transfers=false;
        for(const auto& entry:*entries) {
            // The journal stores UTC; people read their own clock.
            const auto when=QDateTime::fromString(entry.time,Qt::ISODateWithMs).toLocalTime();
            auto item=new QTreeWidgetItem(table,{when.isValid()?when.toString("yyyy-MM-dd HH:mm:ss"):entry.time,entry.operation,entry.outcome,QDir::toNativeSeparators(entry.path),QDir::toNativeSeparators(entry.destination)});
            for(int column=2;column<5;++column) item->setToolTip(column,item->text(column));
            if(entry.attention) item->setForeground(2,Theme::color("@warning"));
            transfers=transfers || !entry.destination.isEmpty();
        }
        // Paths share the remaining width so the table never scrolls sideways; full text is in tooltips.
        table->setColumnHidden(4,!transfers);
        for(int column=0;column<3;++column) table->resizeColumnToContents(column);
        table->setColumnWidth(2,std::min(table->columnWidth(2)+12,380));
        empty->setText(cache->error().isEmpty()?text("No file operations recorded."):cache->error());
        table->setVisible(cache->error().isEmpty() && !entries->empty()); empty->setVisible(!table->isVisibleTo(this));
    };
    fill();
    auto close=new QDialogButtonBox(QDialogButtonBox::Close,this); close->button(QDialogButtonBox::Close)->setText(text("Close")); connect(close,&QDialogButtonBox::rejected,this,&QDialog::reject);
    auto exportHistory=close->addButton(text("Export history…"),QDialogButtonBox::ActionRole);
    connect(exportHistory,&QPushButton::clicked,this,[this,cache] {
        auto path=QFileDialog::getSaveFileName(this,text("Export history…"),{},"CSV (*.csv)");
        if(!path.isEmpty() && !cache->exportOperationHistory(path)) QMessageBox::warning(this,text("Export history…"),text("The export could not be saved."));
    });
    auto clearHistory=close->addButton(text("Clear history…"),QDialogButtonBox::DestructiveRole); clearHistory->setObjectName("clearOperationHistory");
    auto refreshButtons=[exportHistory,clearHistory,cache,entries] { exportHistory->setEnabled(cache->error().isEmpty() && !entries->empty()); clearHistory->setEnabled(exportHistory->isEnabled()); };
    for(auto button:{exportHistory,clearHistory}) button->setAutoDefault(false);
    refreshButtons();
    connect(clearHistory,&QPushButton::clicked,this,[this,cache,fill,refreshButtons,onCleared=std::move(onHistoryCleared)] {
        if(QMessageBox::question(this,text("Clear history…"),text("Remove the recorded file operations? Files are not changed, but Undo last copy or move will no longer be available. Interrupted operations are kept until they are reviewed."),QMessageBox::Yes|QMessageBox::No,QMessageBox::No)!=QMessageBox::Yes) return;
        cache->clearOperationHistory(); fill(); refreshButtons(); if(onCleared) onCleared();
    });
    layout->addWidget(close);
}
