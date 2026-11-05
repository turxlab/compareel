#include "ScanCache.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QSqlQuery>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>
#include <stdexcept>
static void require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        QTemporaryDir root; require(root.isValid(),"temporary root"); const auto path=root.filePath("cache.sqlite");
        VideoFileInfo file; file.fullPath=root.filePath("not-created.mp4").toStdWString(); file.identity={1,2,3,4,true}; file.fileSize=3;
        file.width=640; file.height=480; file.duration100ns=10000000; file.metadataLoaded=file.frameLoaded=true;
        file.sigFirst.valid=true; file.samples.resize(5); file.contentHash=std::string(64,'a');
        ScanCache cache(path); cache.store({file}); auto restored=file;
        require(cache.error().isEmpty() && cache.restore(restored) && restored.frameLoaded && restored.samples.size()==5,"current render evidence");
        cache.recordRemoval(file,E_ACCESSDENIED); cache.ignorePair(file,file);
        {
            auto db=QSqlDatabase::addDatabase("QSQLITE","crop-test"); db.setDatabaseName(path); require(db.open(),"legacy crop fixture connection");
            QSqlQuery query(db); require(query.exec("SELECT payload FROM scan_cache_v4") && query.next(),"read legacy crop payload");
            auto object=QJsonDocument::fromJson(query.value(0).toByteArray().startsWith("CZ1")?qUncompress(query.value(0).toByteArray().mid(3)):query.value(0).toByteArray()).object(); object.remove("evidenceVersion"); query.finish();
            query.prepare("UPDATE scan_cache_v4 SET payload=?"); query.addBindValue(QJsonDocument(object).toJson(QJsonDocument::Compact)); require(query.exec(),"legacy crop payload write");
            db.close();
        }
        QSqlDatabase::removeDatabase("crop-test"); restored=file;
        require(cache.restore(restored) && restored.metadataLoaded && !restored.frameLoaded && restored.samples.empty() && restored.contentHash==file.contentHash,"legacy crop evidence not invalidated or metadata/SHA lost");
        cache.store({file}); require(cache.restore(restored) && restored.frameLoaded,"regenerated crop evidence not reusable");
        {
            auto db=QSqlDatabase::addDatabase("QSQLITE","render-test"); db.setDatabaseName(path); require(db.open(),"mutate fixture connection");
            QSqlQuery query(db); require(query.exec("SELECT payload FROM scan_cache_v4") && query.next(),"read fixture payload");
            auto object=QJsonDocument::fromJson(query.value(0).toByteArray().startsWith("CZ1")?qUncompress(query.value(0).toByteArray().mid(3)):query.value(0).toByteArray()).object(); object.remove("renderVersion"); query.finish();
            query.prepare("UPDATE scan_cache_v4 SET payload=?"); query.addBindValue(QJsonDocument(object).toJson(QJsonDocument::Compact)); require(query.exec(),"old render fixture");
            db.close();
        }
        QSqlDatabase::removeDatabase("render-test"); restored=file;
        require(cache.restore(restored) && !restored.frameLoaded && !restored.metadataLoaded && restored.samples.empty() && restored.thumbBGRA.empty() && restored.contentHash==file.contentHash,"old HDR/rotation evidence reused or SHA lost");
        require(cache.operationHistory().size()==1 && cache.ignoredPairs().size()==1,"review history lost");
        cache.store({file}); require(cache.restore(restored) && restored.frameLoaded && restored.samples.size()==5,"new evidence not reusable");
        {
            ScanCache large(root.filePath("large.sqlite")); std::vector<VideoFileInfo> records;
            for(int i=0;i<10001;++i) { auto item=file; item.fullPath+=std::to_wstring(i); item.identity.index=i+100; records.push_back(std::move(item)); }
            large.store(records); auto first=records.front(),last=records.back();
            require(large.error().isEmpty() && large.restore(first) && large.restore(last) && first.contentHash==file.contentHash,"compressed cache must retain more than 10000 entries without changing evidence");
        }
        {
            auto db=QSqlDatabase::addDatabase("QSQLITE","bounded-cache"); db.setDatabaseName(path); require(db.open(),"compressed fixture connection");
            QSqlQuery query(db); auto bomb=qCompress(QByteArray(131073,'x')); qToBigEndian<quint32>(1,bomb.data());
            query.prepare("UPDATE scan_cache_v4 SET payload=?"); query.addBindValue(QByteArray("CZ1")+bomb); require(query.exec(),"bounded cache fixture");
            restored=file; require(!cache.restore(restored) && restored.contentHash==file.contentHash,"compressed cache must reject expansion beyond declared bound");
            db.close();
        }
        QSqlDatabase::removeDatabase("bounded-cache");
        puts("PASS render cache: old pixels/metadata invalidated, SHA/history/decisions preserved, new evidence reused; no media"); return 0;
    } catch(const std::exception& error) { fprintf(stderr,"FAIL %s\n",error.what()); return 1; }
}
