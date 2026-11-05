#include <cstdlib>
#include "ScanCache.h"
#include "platform/Platform.h"
#include "FileOperations.h"
#include <QCoreApplication>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <stdexcept>

static void require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }

int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    Platform::ComInit com(Platform::ComApartment::ApartmentThreaded);
    int status=0;
    try {
        require(com.succeeded(),"COM initialization");
        const auto args=app.arguments();
        if(args.size()==4 && args[1]=="--transfer-child") {
            bool ok=false; const int stage=args[2].toInt(&ok); require(ok && stage>=1 && stage<=6,"transfer child stage");
            const auto source=args[3]+"/source.txt",destination=args[3]+"/copy.txt";
            QFile input(source); require(input.open(QIODevice::WriteOnly|QIODevice::NewOnly) && input.write("private transfer fixture")==24,"transfer source fixture"); input.close();
            VideoFileInfo file; file.fullPath=source.toStdWString(); file.identity=FileOperations::ReadIdentity(file.fullPath);
            ScanCache cache(args[3]+"/cache.sqlite"); const auto id=cache.beginTransfer(file,destination,"copy"); require(id>0,"durable transfer intent");
            std::atomic<bool> cancel{false}; FileOperations::TransferResult copied;
            if(stage>=2) { copied=FileOperations::Transfer(file.fullPath,destination.toStdWString(),FileOperations::TransferKind::Copy,file.identity,cancel); require(copied.succeeded(),"actual journaled copy"); }
            if(stage>=3) { cache.completeTransfer(id,copied); require(cache.error().isEmpty(),"transfer completion commit"); }
            if(stage>=4) require(cache.beginTransferUndo(id),"undo intent commit");
            if(stage>=5) {
                const auto undone=FileOperations::UndoTransfer(file.fullPath,destination.toStdWString(),FileOperations::TransferKind::Copy,file.identity,copied.destinationIdentity,copied.contentHash,cancel);
                if(!undone.succeeded()) throw std::runtime_error(QString("actual journaled copy undo: 0x%1 %2").arg(quint32(undone.error),8,16,QChar('0')).arg(QString::fromStdWString(undone.message())).toStdString());
                if(stage==6) { cache.completeTransferUndo(id,undone); require(cache.error().isEmpty(),"undo completion commit"); }
            }
            std::_Exit(73); return 1;
        }
        if(args.size()==4 && args[1]=="--child") {
            bool ok=false; const auto stage=args[2].toInt(&ok); require(ok && stage>=1 && stage<=3,"child stage");
            const auto path=args[3]+"/disposable.txt";
            QFile fixture(path); require(fixture.open(QIODevice::WriteOnly|QIODevice::NewOnly),"new private fixture");
            const QByteArray bytes="Compareel disposable operation-recovery fixture";
            require(fixture.write(bytes)==bytes.size(),"fixture write"); fixture.close();
            VideoFileInfo file; file.fullPath=QDir::toNativeSeparators(path).toStdWString();
            file.identity=FileOperations::ReadIdentity(file.fullPath); file.fileSize=file.identity.size;
            require(file.identity.valid,"fixture identity");
            ScanCache cache(args[3]+"/cache.sqlite"); const auto intent=cache.beginRemoval(file);
            require(intent>0 && cache.error().isEmpty(),"durable removal intent");
            if(stage>=2) {
                const auto removed=FileOperations::Remove(nullptr,file.fullPath,true,file.identity);
                require(removed.succeeded() && !QFile::exists(path),"real private fixture recycling");
                if(stage==3) { cache.completeRemoval(intent,S_OK); require(cache.error().isEmpty(),"durable completed result"); }
            }
            // Deliberately bypass SQLite/Qt destructors after each real operation boundary.
            std::_Exit(73);
            return 1;
        }
        require(args.size()==1,"unexpected arguments");
        QTemporaryDir root; require(root.isValid(),"private test root");
        for(int stage=1;stage<=3;++stage) {
            const auto directory=root.path()+"/stage-"+QString::number(stage);
            require(QDir().mkpath(directory),"private stage folder");
            QProcess child; child.start(QCoreApplication::applicationFilePath(),{"--child",QString::number(stage),directory});
            require(child.waitForFinished(15000) && child.exitCode()==73,"abrupt operation child");
            ScanCache reopened(directory+"/cache.sqlite"); const auto history=reopened.operationHistory();
            require(reopened.error().isEmpty() && history.size()==1,"journal reopen");
            require(history.front().contains(stage==3?"Completed":"Outcome unconfirmed"),"completion must not be inferred from path absence");
            require(QFile::exists(directory+"/disposable.txt")==bool(stage==1),"real filesystem state");
            if(stage==1) {
                QFile file(directory+"/disposable.txt"); require(file.open(QIODevice::ReadOnly) && file.readAll()=="Compareel disposable operation-recovery fixture","pre-operation crash changed source");
            }
        }
        puts("PASS operation recovery: real private Trash at intent/pre-completion/post-completion crash boundaries; reopened pending stays unconfirmed even when path is absent; completed result retained; user files untouched");
        for(int stage=1;stage<=6;++stage) {
            const auto directory=root.path()+"/transfer-"+QString::number(stage); require(QDir().mkpath(directory),"transfer stage directory");
            QProcess child; child.start(QCoreApplication::applicationFilePath(),{"--transfer-child",QString::number(stage),directory});
            if(!child.waitForFinished(15000) || child.exitCode()!=73)
                throw std::runtime_error(QString("abrupt transfer child stage %1, exit %2: %3").arg(stage).arg(child.exitCode()).arg(QString::fromUtf8(child.readAllStandardError())).toStdString());
            ScanCache reopened(directory+"/cache.sqlite"); const auto undo=reopened.lastUndoableTransfer(); const auto history=reopened.operationHistory();
            require(reopened.error().isEmpty() && history.size()==1 && bool(undo)==bool(stage==3),"interrupted transfer or undo became automatically undoable");
            require(history.front().contains(stage==6?"Undone":stage==3?"Completed":"Outcome unconfirmed"),"transfer recovery phase");
            require(QFile::exists(directory+"/source.txt") && QFile::exists(directory+"/copy.txt")==bool(stage>=2 && stage<=4),"transfer recovery filesystem state");
            require(reopened.exportOperationHistory(directory+"/history.csv"),"transfer history export");
            QFile csv(directory+"/history.csv"); require(csv.open(QIODevice::ReadOnly),"read transfer history export"); const auto bytes=csv.readAll();
            require(bytes.startsWith(QByteArray::fromHex("efbbbf")) && bytes.contains("destination,phase,undo_hresult,copied,moved") && bytes.contains("copy.txt"),"transfer CSV fields");
            if(stage==3) {
                reopened.clearScanData(); require(reopened.error().isEmpty() && reopened.lastUndoableTransfer().has_value(),"media clear lost transfer history");
                VideoFileInfo source; source.fullPath=undo->source.toStdWString(); source.identity=undo->sourceIdentity;
                require(reopened.beginTransfer(source,directory+"/another.txt","copy")>0 && !reopened.lastUndoableTransfer(),"unconfirmed newer operation must block older Undo");
            }
        }
        puts("PASS transfer journal: six actual copy/undo abrupt-exit boundaries, pending remains unconfirmed, completed identity/hash survives, Undo intent prevents retry, CSV and media-clear preserve history; private files only");
        {
            ScanCache cache(root.filePath("cancelled.sqlite"));
            VideoFileInfo file; file.fullPath=root.filePath("cancelled.txt").toStdWString();
            QFile source(QString::fromStdWString(file.fullPath)); require(source.open(QIODevice::WriteOnly) && source.write("cancelled")==9,"cancelled source"); source.close();
            file.identity=FileOperations::ReadIdentity(file.fullPath);
            const auto id=cache.beginTransfer(file,root.filePath("never-created.txt"),"move"); require(id>0,"cancelled transfer intent");
            std::atomic<bool> cancel{true}; const auto result=FileOperations::Transfer(file.fullPath,root.filePath("never-created.txt").toStdWString(),FileOperations::TransferKind::Move,file.identity,cancel);
            cache.completeTransfer(id,result);
            require(cache.error().isEmpty() && !cache.lastUndoableTransfer() && cache.operationHistory().front().contains("HRESULT"),"failed transfer must persist without a destination identity");
        }
#ifdef _WIN32
        // An open file that blocks removal is how a partial move is forced; POSIX has no such locks.
        {
            // A partial move (copy kept, source not removed) only happens across volumes; force that path on this disk.
            FileOperations::sameVolumeRename=false;
            const auto path=root.filePath("partial-source.txt"),destination=root.filePath("partial-destination.txt"),database=root.filePath("partial.sqlite");
            QFile source(path); require(source.open(QIODevice::WriteOnly) && source.write("partial transfer fixture")==24,"partial fixture"); source.close();
            VideoFileInfo file; file.fullPath=path.toStdWString(); file.identity=FileOperations::ReadIdentity(file.fullPath);
            std::atomic<bool> cancel{false};
            HANDLE lock=CreateFileW(file.fullPath.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr); require(lock!=INVALID_HANDLE_VALUE,"partial move lock");
            qint64 id=0;
            {
                ScanCache cache(database); id=cache.beginTransfer(file,destination,"move"); require(id>0,"partial move intent");
                const auto result=FileOperations::Transfer(file.fullPath,destination.toStdWString(),FileOperations::TransferKind::Move,file.identity,cancel);
                CloseHandle(lock); cache.completeTransfer(id,result);
                require(!result.succeeded() && result.copied && !result.moved && cache.error().isEmpty(),"partial move journal flags");
            }
            {
                ScanCache cache(database); const auto record=cache.lastUndoableTransfer(); require(record && !record->moved && record->copied && cache.beginTransferUndo(id),"reopened partial move must undo Copy");
                const auto result=FileOperations::UndoTransfer(record->source.toStdWString(),record->destination.toStdWString(),FileOperations::TransferKind::Copy,record->sourceIdentity,record->destinationIdentity,record->contentHash,cancel);
                cache.completeTransferUndo(id,result); require(result.succeeded() && cache.error().isEmpty() && !cache.lastUndoableTransfer() && QFile::exists(path) && !QFile::exists(destination),"partial move Copy Undo");
                id=cache.beginTransfer(file,destination,"move");
                const auto moved=FileOperations::Transfer(file.fullPath,destination.toStdWString(),FileOperations::TransferKind::Move,file.identity,cancel);
                cache.completeTransfer(id,moved); require(moved.succeeded() && cache.beginTransferUndo(id),"actual Move before partial Undo");
                lock=CreateFileW(destination.toStdWString().c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr); require(lock!=INVALID_HANDLE_VALUE,"partial Undo lock");
                const auto partial=FileOperations::UndoTransfer(file.fullPath,destination.toStdWString(),FileOperations::TransferKind::Move,file.identity,moved.destinationIdentity,moved.contentHash,cancel);
                CloseHandle(lock); cache.completeTransferUndo(id,partial);
                require(!partial.succeeded() && partial.copied && !partial.moved && partial.destinationIdentity.valid && cache.error().isEmpty(),"partial Undo persists restored identity");
            }
            {
                ScanCache cache(database); const auto record=cache.lastUndoableTransfer();
                require(record && record->moved && record->restoredIdentity.valid && cache.beginTransferUndo(id),"restart partial Undo ownership");
                const auto result=FileOperations::UndoTransfer(record->source.toStdWString(),record->destination.toStdWString(),FileOperations::TransferKind::Copy,record->restoredIdentity,record->destinationIdentity,record->contentHash,cancel);
                cache.completeTransferUndo(id,result);
                require(result.succeeded() && cache.error().isEmpty() && !cache.lastUndoableTransfer() && QFile::exists(path) && !QFile::exists(destination),"explicit partial Undo retry preserves restored source");
            }
            FileOperations::sameVolumeRename=true;
        }
#endif
        puts("PASS partial transfer recovery: failed no-target result persists, newer pending blocks older Undo, partial Move undoes Copy, partial Move-Undo restored identity survives reopening and explicit verified retry; private files only");
        {
            // Permanent journal failure must block destructive Removal/Transfer before touching disk
            const auto guardedPath=root.filePath("guarded-source.txt");
            QFile guarded(guardedPath); require(guarded.open(QIODevice::WriteOnly) && guarded.write("must survive journal failure")==28,"guarded source fixture"); guarded.close();
            VideoFileInfo guardedInfo; guardedInfo.fullPath=QDir::toNativeSeparators(guardedPath).toStdWString();
            guardedInfo.identity=FileOperations::ReadIdentity(guardedInfo.fullPath);
            require(guardedInfo.identity.valid,"guarded source identity");
            // Point ScanCache at an invalid path (where a file blocks directory creation)
            const auto blockerFile=root.filePath("not-a-dir");
            QFile blocker(blockerFile); require(blocker.open(QIODevice::WriteOnly) && blocker.write("x")==1,"blocker file"); blocker.close();
            ScanCache brokenCache(blockerFile+"/sub/cache.sqlite");
            const auto badRemovalId=brokenCache.beginRemoval(guardedInfo);
            require(badRemovalId<=0 && !brokenCache.error().isEmpty(),"broken journal must reject beginRemoval");
            const auto badTransferId=brokenCache.beginTransfer(guardedInfo,root.filePath("dest.txt"),"move");
            require(badTransferId<=0 && !brokenCache.error().isEmpty(),"broken journal must reject beginTransfer");
            require(QFile::exists(guardedPath) && FileOperations::ReadIdentity(guardedInfo.fullPath)==guardedInfo.identity,"guarded file untouched when journal fails");
        }
        puts("PASS journal failure guard: unavailable SQLite journal blocks removal/transfer intent before any file mutation");
    } catch(const std::exception& error) { fprintf(stderr,"FAIL %s\n",error.what()); status=1; }
    return status;
}
