#include "MainWindow.h"
#include "Language.h"
#include "FileOperations.h"
#include "ScanCache.h"
#include "platform/Platform.h"
#include "PathKey.h"
#include <QAction>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QMenuBar>
#include <QTimer>
#include <QTemporaryDir>
#include <QFile>
#include <QApplication>
#include <QThread>
#include <QStyledItemDelegate>
#include <QProgressDialog>
#include <QPushButton>
#include <QStatusBar>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
#include <set>

using Language::text;
namespace {
class TargetNameDelegate : public QStyledItemDelegate {
public: using QStyledItemDelegate::QStyledItemDelegate;
    QWidget* createEditor(QWidget* parent,const QStyleOptionViewItem& option,const QModelIndex& index) const override { return index.column()==1?QStyledItemDelegate::createEditor(parent,option,index):nullptr; }
};
bool validName(const QString& name) { return Platform::IsValidFileName(name.toStdWString()); }
}
void MainWindow::chooseBatchTransfer(bool move,const QString& suppliedFolder) {
    if(scanning_ || sortingMetadata_ || fileOperationPending_ || clearingCache_ || closing_ || groups_.empty()) return;
    std::vector<VideoFileInfo> jobs; std::set<std::wstring> seen; unsigned excluded=0;
    for(const auto& group:groups_) for(int i:group.fileIndices) {
        const auto& f=files_[i]; if(f.deleted || !f.identity.valid || !seen.insert(PhysicalFileKey(f)).second) continue;
        if(move && (f.protectedFile || f.protectedByFolder || protectedByReview(f))) { ++excluded; continue; }
        VideoFileInfo job; job.fullPath=f.fullPath; job.fileName=f.fileName; job.identity=f.identity; job.fileSize=f.fileSize; jobs.push_back(std::move(job));
    }
    if(jobs.empty()) { QMessageBox::information(this,text("Transfer results"),text("No eligible files.")); return; }
    const auto folder=suppliedFolder.isEmpty()?QFileDialog::getExistingDirectory(this,text("Transfer results"),settings_.value("transferFolder").toString()):suppliedFolder;
    if(folder.isEmpty()) return;
    QDialog dialog(this); dialog.setObjectName("batchTransferPlan"); dialog.setWindowTitle(text(move?"Move all result files…":"Copy all result files…")); dialog.resize(760,480);
    auto layout=new QVBoxLayout(&dialog);
    auto summary=new QLabel(text("All groups, all members: %1 files. Protected files excluded from move: %2. No overwrite.").arg(jobs.size()).arg(excluded),&dialog); summary->setWordWrap(true); layout->addWidget(summary);
    auto policy=new QComboBox(&dialog); policy->setObjectName("batchRenamePolicy"); policy->addItems({text("Rename all conflicts: _1"),text("Rename all conflicts: (1)"),text("Manual names (edit target column)")}); layout->addWidget(policy);
    auto table=new QTreeWidget(&dialog); table->setObjectName("batchTransferFiles"); table->setRootIsDecorated(false); table->setHeaderLabels({text("Source"),text("Target filename")}); table->header()->setSectionResizeMode(0,QHeaderView::Stretch); table->header()->setSectionResizeMode(1,QHeaderView::ResizeToContents); layout->addWidget(table);
    table->setItemDelegate(new TargetNameDelegate(table));
    for(const auto& job:jobs) { auto row=new QTreeWidgetItem(table,{QString::fromStdWString(job.fullPath),QString::fromStdWString(job.fileName)}); row->setFlags(row->flags()|Qt::ItemIsEditable); }
    auto rename=[&] {
        if(policy->currentIndex()==2) return;
        QSet<QString> reserved;
        for(int i=0;i<table->topLevelItemCount();++i) {
            const auto original=QString::fromStdWString(jobs[size_t(i)].fileName); QFileInfo info(original);
            QString name=original; int n=0;
            while(reserved.contains(pathKey(name)) || QFileInfo::exists(QDir(folder).filePath(name))) {
                const auto suffix=policy->currentIndex()==0?QString("_%1").arg(++n):QString(" (%1)").arg(++n);
                name=info.completeBaseName()+suffix+(info.suffix().isEmpty()?QString():"."+info.suffix());
            }
            reserved.insert(pathKey(name)); table->topLevelItem(i)->setText(1,name);
        }
    };
    connect(policy,&QComboBox::currentIndexChanged,&dialog,[&] { rename(); }); rename();
    auto buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&dialog); layout->addWidget(buttons);
    connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    QStringList destinations;
    connect(buttons,&QDialogButtonBox::accepted,&dialog,[&] {
        QSet<QString> reserved; destinations.clear();
        for(int i=0;i<table->topLevelItemCount();++i) {
            const auto name=table->topLevelItem(i)->text(1); const auto path=QDir(folder).absoluteFilePath(name);
            if(!validName(name) || reserved.contains(pathKey(name)) || QFileInfo::exists(path)) { QMessageBox::warning(&dialog,text("Transfer results"),text("Invalid or occupied target name. Edit the target column or choose Rename all.")); return; }
            reserved.insert(pathKey(name)); destinations.push_back(path);
        }
        dialog.accept();
    });
    if(dialog.exec()!=QDialog::Accepted) return;
    if(!beginCleanupRecovery()) return;
    settings_.setValue("transferFolder",folder); bulkCancel_.store(false); syncing_=false;
    auto oldLeft=left_->detachPlayer(),oldRight=right_->detachPlayer(); fileOperationPending_=true; centralWidget()->setEnabled(false); menuBar()->setEnabled(false);
    auto progress=new QProgressDialog(text("Copying and verifying…"),text("Cancel"),0,int(jobs.size()),this); progress->setWindowModality(Qt::WindowModal); progress->setMinimumDuration(0); progress->setAutoClose(false); progress->show();
    connect(progress,&QProgressDialog::canceled,this,[this] { bulkCancel_.store(true); });
    struct Result { std::vector<int> moved; unsigned copied=0; QString error; };
    auto watcher=new QFutureWatcher<Result>(this); batchTransferWatcher_=watcher;
    connect(watcher,&QFutureWatcherBase::finished,this,[this,watcher,progress,jobs] {
        const auto result=watcher->result(); watcher->deleteLater(); batchTransferWatcher_=nullptr; progress->hide(); progress->deleteLater();
        fileOperationPending_=false; centralWidget()->setEnabled(true); menuBar()->setEnabled(true);
        for(int i:result.moved) for(auto& f:files_) if(f.fullPath==jobs[size_t(i)].fullPath && f.identity==jobs[size_t(i)].identity) f.deleted=true;
        if(result.error.isEmpty()) finishCleanupRecovery();
        if(!result.moved.empty()) refreshTransferResults(); else if(currentGroup_>=0) selectGroup(currentGroup_);
        updateUndoAction(); statusBar()->showMessage(text("Copied: %1 · moved: %2. See operation history; Undo applies one file at a time.").arg(result.copied).arg(result.moved.size()));
        if(!result.error.isEmpty()) {
            QMessageBox box(QMessageBox::Warning,text("Transfer results"),result.error,QMessageBox::Ok,this);
            box.setTextFormat(Qt::PlainText);
            box.exec();
        }
        if(pendingClose_) close();
    });
    watcher->setFuture(QtConcurrent::run([this,jobs=std::move(jobs),destinations,move,oldLeft,oldRight,progress] {
        Result result; Platform::ComInit com(Platform::ComApartment::ApartmentThreaded);
        oldLeft->Close(); oldRight->Close(); previewPool_.waitForDone();
        try {
            ScanCache journal(cachePath_);
            for(size_t i=0;i<jobs.size() && !bulkCancel_.load();++i) {
                if(!com.succeeded()) { result.error=text("COM initialization failed"); break; }
                const auto id=journal.beginTransfer(jobs[i],destinations[int(i)],move?"move":"copy");
                if(id<=0) { result.error=journal.error(); if(result.error.isEmpty()) result.error=text("Transfer journal unavailable"); break; }
                const auto outcome=FileOperations::Transfer(jobs[i].fullPath,destinations[int(i)].toStdWString(),move?FileOperations::TransferKind::Move:FileOperations::TransferKind::Copy,jobs[i].identity,bulkCancel_);
                journal.completeTransfer(id,outcome);
                if(outcome.copied) ++result.copied; if(outcome.moved) result.moved.push_back(int(i));
                QMetaObject::invokeMethod(progress,[progress,done=int(i)+1] { progress->setValue(done); },Qt::QueuedConnection);
                if(!journal.error().isEmpty() || !outcome.succeeded()) { result.error=QString::fromStdWString(outcome.message())+"\n"+journal.error(); break; }
            }
        } catch(...) { result.error=text("Transfer interrupted; inspect operation history and both paths before retrying."); }
        return result;
    }));
}
bool MainWindow::runBatchTransferCheck() {
    QTemporaryDir root; if(!root.isValid()) return false;
    for(const auto& folder:{"one","two","three","copy","move"}) QDir().mkpath(root.filePath(folder));
    const QStringList sources{root.filePath("one/a.png"),root.filePath("two/a.png"),root.filePath("three/b.png")};
    QImage image(32,32,QImage::Format_RGB32); image.fill(Qt::red);
    files_.clear(); groups_.clear(); manual_=false; currentGroup_=-1;
    for(const auto& path:sources) { if(!image.save(path)) return false; VideoFileInfo f; f.fullPath=path.toStdWString(); f.fileName=QFileInfo(path).fileName().toStdWString(); f.identity=FileOperations::ReadIdentity(f.fullPath); f.fileSize=f.identity.size; files_.push_back(f); }
    VideoGroup group; group.fileIndices={0,1,2}; groups_={group,group};
    if(!image.save(root.filePath("copy/a.png"))) return false;
    bool renamed=false; QTimer accept; connect(&accept,&QTimer::timeout,this,[&] {
        auto dialog=findChild<QDialog*>("batchTransferPlan"); if(!dialog) return;
        auto names=dialog->findChild<QTreeWidget*>("batchTransferFiles"); auto policy=dialog->findChild<QComboBox*>("batchRenamePolicy");
        if(names->topLevelItemCount()!=3) { dialog->reject(); return; }
        renamed=names->topLevelItem(0)->text(1)=="a_1.png" && names->topLevelItem(1)->text(1)=="a_2.png";
        policy->setCurrentIndex(1); renamed=renamed && names->topLevelItem(0)->text(1)=="a (1).png" && names->topLevelItem(1)->text(1)=="a (2).png";
        policy->setCurrentIndex(2); names->topLevelItem(1)->setText(1,"manual.png"); dialog->findChild<QDialogButtonBox*>()->accepted();
    }); accept.start(5); chooseBatchTransfer(false,root.filePath("copy")); accept.stop();
    auto wait=[&] { QElapsedTimer timer; timer.start(); while(fileOperationPending_ && timer.elapsed()<10000) { QApplication::processEvents(); QThread::msleep(2); } return !fileOperationPending_; };
    if(!wait() || !renamed || !QFileInfo::exists(root.filePath("copy/a (1).png")) || !QFileInfo::exists(root.filePath("copy/manual.png")) || !QFileInfo::exists(root.filePath("copy/b.png"))) { qCritical()<<"FAIL batch transfer: copy plan, renamed"<<renamed<<"pending"<<fileOperationPending_; return false; }
    for(const auto& path:sources) if(!QFileInfo::exists(path)) { qCritical("FAIL batch transfer: copy removed a source"); return false; }
    files_[0].protectedFile=true;
    QTimer confirm; connect(&confirm,&QTimer::timeout,this,[&] { if(auto dialog=findChild<QDialog*>("batchTransferPlan")) dialog->findChild<QDialogButtonBox*>()->accepted(); }); confirm.start(5);
    chooseBatchTransfer(true,root.filePath("move")); confirm.stop();
    if(!wait() || !QFileInfo::exists(sources[0]) || QFileInfo::exists(sources[1]) || QFileInfo::exists(sources[2]) || !QFileInfo::exists(root.filePath("move/a.png"))) { qCritical()<<"FAIL batch transfer: protected move, pending"<<fileOperationPending_; return false; }
    ScanCache journal(cachePath_); if(!journal.lastUndoableTransfer()) { qCritical("FAIL batch transfer: Undo history"); return false; }
    qInfo()<<"PASS batch transfer actual plan/UI/worker: all three members, identity dedup, existing name preservation, both Rename-all styles, manual name, verified copy and protected move, durable Undo history; private generated files";
    return true;
}
