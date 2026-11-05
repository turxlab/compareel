#include <cstdlib>
#include "VideoScanner.h"
#include "platform/Platform.h"
#include "ScanCache.h"
#include "FileOperations.h"
#include <QCoreApplication>
#include <QProcess>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QThread>
#include <QFileInfo>
#include <QDir>
#include <QSqlQuery>
#include <QUuid>
#include <stdexcept>
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv); auto args=app.arguments();
    Platform::ComInit com; Platform::MediaRuntime media; int result=0;
    try {
        if(args.size()<2 || !com.succeeded() || !media.succeeded()) throw std::runtime_error("usage: CacheCrashChecks media | --child media cache");
        bool child=args[1]=="--child"; QString path=QFileInfo(args[child?2:1]).absoluteFilePath();
        VideoFileInfo file; file.fullPath=QDir::toNativeSeparators(path).toStdWString(); file.identity=FileOperations::ReadIdentity(file.fullPath); file.fileSize=file.identity.size;
        if(!file.identity.valid) throw std::runtime_error("fixture identity");
        if(child) {
            VideoScanner::ExtractMetadataOnly(file); VideoScanner::ExtractMetadataAndFrame(file,true);
            CacheWriter writer(args[3]); writer.submit(file); ScanCache reader(args[3]);
            QElapsedTimer timeout; timeout.start(); bool committed=false;
            while(timeout.elapsed()<10000) {
                auto restored=file; restored.frameLoaded=false; restored.samples.clear();
                if(reader.restore(restored) && restored.samples.size()==5 && restored.samples[0].normalized.size()==8) { committed=true; break; }
                QThread::msleep(10);
            }
            if(!committed || reader.beginRemoval(file)<=0) throw std::runtime_error("checkpoint before abrupt exit");
            // Deliberately bypass destructors, writer.finish() and normal application shutdown.
            std::_Exit(73); return 74;
        }
        QTemporaryDir state; if(!state.isValid()) throw std::runtime_error("temporary state");
        auto db=state.filePath("checkpoint.sqlite"); QProcess worker;
        worker.start(QCoreApplication::applicationFilePath(),{"--child",path,db});
        if(!worker.waitForFinished(15000) || worker.exitCode()!=73) throw std::runtime_error("abrupt child exit");
        ScanCache reopened(db);
        if(!reopened.restore(file) || file.samples.size()!=5 || file.samples[0].normalized.size()!=8 || !reopened.operationHistory().join(' ').contains("Outcome unconfirmed") || FileOperations::ReadIdentity(file.fullPath)!=file.identity)
            throw std::runtime_error("restart lost checkpoint/intent or touched source");
        auto connection=QUuid::createUuid().toString();
        std::atomic<bool> cancelled{false}; HRESULT hashError=S_OK;
        file.contentHash=FileOperations::ContentHash(file.fullPath,cancelled,hashError);
        if(FAILED(hashError)) throw std::runtime_error("migration hash fixture");
        reopened.store({file});
        {
            auto sql=QSqlDatabase::addDatabase("QSQLITE",connection); sql.setDatabaseName(db);
            if(!sql.open()) throw std::runtime_error("migration fixture open");
            QSqlQuery query(sql);
            if(!query.exec("CREATE TABLE scan_cache_v3 AS SELECT * FROM scan_cache_v4") || !query.exec("DELETE FROM scan_cache_v4")) throw std::runtime_error("migration fixture");
            sql.close();
        }
        QSqlDatabase::removeDatabase(connection);
        ScanCache migrated(db); auto old=file;
        if(!migrated.restore(old) || old.frameLoaded || old.sigFirst.valid || !old.samples.empty() || !old.thumbBGRA.empty() || old.contentHash!=file.contentHash || migrated.operationHistory().empty())
            throw std::runtime_error("old evidence reused, or valid metadata/hash/history lost");
        reopened.store({file});
        if(!reopened.restore(old)) throw std::runtime_error("new reader cache not restored");
        reopened.clearScanData();
        if(!reopened.error().isEmpty() || reopened.restore(old) || reopened.operationHistory().empty()) throw std::runtime_error("clear media cache lost history");
        {
            auto sql=QSqlDatabase::addDatabase("QSQLITE",connection); sql.setDatabaseName(db);
            if(!sql.open()) throw std::runtime_error("budget fixture open");
            QSqlQuery query(sql);
            if(!query.exec("WITH RECURSIVE rows(n) AS (VALUES(1) UNION ALL SELECT n+1 FROM rows WHERE n<9000) "
                "INSERT INTO scan_cache_v4 SELECT 'budget-'||n,'',ZEROBLOB(32768) FROM rows")) throw std::runtime_error("budget fixture insert");
            reopened.store({file});
            if(!reopened.error().isEmpty() || !query.exec("SELECT COUNT(*),SUM(LENGTH(payload)) FROM scan_cache_v4") || !query.next() ||
                query.value(0).toLongLong()>ScanCache::MaxMediaEntries || query.value(1).toLongLong()>ScanCache::MaxMediaPayloadBytes || !reopened.restore(old) || reopened.operationHistory().empty())
                throw std::runtime_error("media payload budget or retained newest/history");
            sql.close();
        }
        QSqlDatabase::removeDatabase(connection);
        puts("PASS abrupt restart: committed normalized signatures restored without writer shutdown; pending removal remains unconfirmed; source untouched");
        puts("PASS cache migration: old frame evidence invalidated, identity-valid metadata/SHA preserved; v4 refreshed; clear cache keeps history");
        puts("PASS cache budget: 9000 rows above 256MiB pruned; newest evidence and operation history retained");
    } catch(const std::exception& error) { fprintf(stderr,"FAIL %s\n",error.what()); result=1; }
    return result;
}
