#include "ScanCache.h"
#include "PathKey.h"
#include "VideoScanner.h"
#include <QFile>
#include <QFileInfo>
#include "Csv.h"
#include "Language.h"
#include <QByteArray>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QSaveFile>
#include <QSet>
#include <QtEndian>
#if __has_include(<QtZlib/zlib.h>)
#include <QtZlib/zlib.h>
#else
#include <zlib.h>
#endif

namespace {
QString number(uint64_t value) { return QString::number(value,16); }
QString key(const VideoFileInfo& file) { return pathKey(QDir::cleanPath(QString::fromStdWString(file.fullPath))); }
QString identity(const VideoFileInfo& file) {
    const auto& i=file.identity;
    return number(i.volume)+":"+number(i.index)+":"+number(i.size)+":"+number(i.modified);
}
QString identity(const FileIdentity& value) { VideoFileInfo file; file.identity=value; return value.valid?identity(file):QStringLiteral(""); }
bool parseIdentity(const QString& text,FileIdentity& value) {
    value={}; if(text.isEmpty()) return true;
    const auto parts=text.split(':'); if(parts.size()!=4) return false;
    bool ok=false; uint64_t* fields[]={&value.volume,&value.index,&value.size,&value.modified};
    for(int i=0;i<4;++i) { *fields[i]=parts[i].toULongLong(&ok,16); if(!ok) return false; }
    value.valid=true; return true;
}
QJsonObject signature(const FrameSignature& value) {
    QJsonArray normalized; for(const auto& item:value.normalized) normalized.append(signature(item));
    return {{"normalized",normalized},{"h",number(value.dHashH)},{"v",number(value.dHashV)},{"a",number(value.aHash)},
        {"grid",QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(value.rgbGrid),48).toBase64())},
        {"spatial",value.hasSpatialDetail?QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(value.spatialLuma),144).toBase64()):QString()},
        {"cspatial",value.hasSpatialDetail?QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(value.centerSpatialLuma),144).toBase64()):QString()},
        {"valid",value.valid},{"blank",value.lowVariance},{"time",QString::number(value.timestamp100ns)},
        {"subj",value.hasSubjectDetail},{"sh",number(value.subjDHashH)},{"sv",number(value.subjDHashV)},{"sa",number(value.subjAHash)},
        {"eh",number(value.edgeHashH)},{"ev",number(value.edgeHashV)},{"ea",number(value.edgeAHash)},{"fm64",number(value.fgCellMask64)},
        {"sgrid",value.hasSubjectDetail?QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(value.subjRgbGrid),48).toBase64()):QString()},
        {"sfm",int(value.subjFgMask)},{"bgc",int(value.bgCoherencePermille)},
        {"crop",value.hasCenterCrop},{"ch",number(value.centerDHashH)},{"cv",number(value.centerDHashV)},{"ca",number(value.centerAHash)},
        {"cgrid",value.hasCenterCrop?QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(value.centerRgbGrid),48).toBase64()):QString()},
        {"csh",number(value.centerSubjDHashH)},{"csv",number(value.centerSubjDHashV)},{"csa",number(value.centerSubjAHash)},
        {"ceh",number(value.centerEdgeHashH)},{"cev",number(value.centerEdgeHashV)},{"cea",number(value.centerEdgeAHash)},{"cfm64",number(value.centerFgCellMask64)},
        {"csgrid",value.hasCenterCrop?QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(value.centerSubjRgbGrid),48).toBase64()):QString()},
        {"csfm",int(value.centerSubjFgMask)},{"cbgc",int(value.centerBgCoherencePermille)}};
}
bool signature(const QJsonObject& object,FrameSignature& value,bool nested=false) {
    auto normalized=object["normalized"].toArray();
    if((nested && !normalized.empty()) || (!normalized.empty() && normalized.size()!=8)) return false;
    value.normalized.clear(); for(auto item:normalized) { FrameSignature sub; if(!item.isObject() || !signature(item.toObject(),sub,true)) return false; value.normalized.push_back(std::move(sub)); }
    bool h=false,v=false,a=false;
    value.dHashH=object["h"].toString().toULongLong(&h,16);
    value.dHashV=object["v"].toString().toULongLong(&v,16);
    value.aHash=object["a"].toString().toULongLong(&a,16);
    auto bytes=QByteArray::fromBase64(object["grid"].toString().toLatin1());
    bool time=false; value.timestamp100ns=object["time"].toString().toLongLong(&time);
    if(!h || !v || !a || !time || bytes.size()!=48) return false;
    memcpy(value.rgbGrid,bytes.constData(),48); value.valid=object["valid"].toBool(); value.lowVariance=object["blank"].toBool();
    const auto spatial=QByteArray::fromBase64(object["spatial"].toString().toLatin1());
    const auto centerSpatial=QByteArray::fromBase64(object["cspatial"].toString().toLatin1());
    value.hasSpatialDetail=false;
    if(!spatial.isEmpty() || !centerSpatial.isEmpty()) {
        if(spatial.size()!=144 || centerSpatial.size()!=144) return false;
        memcpy(value.spatialLuma,spatial.constData(),144); memcpy(value.centerSpatialLuma,centerSpatial.constData(),144); value.hasSpatialDetail=true;
    }
    value.hasSubjectDetail=false;
    if(object["subj"].toBool()) {
        bool sh=false,sv=false,sa=false,eh=false,ev=false,ea=false,fm=false;
        value.subjDHashH=object["sh"].toString().toULongLong(&sh,16);
        value.subjDHashV=object["sv"].toString().toULongLong(&sv,16);
        value.subjAHash=object["sa"].toString().toULongLong(&sa,16);
        value.edgeHashH=object["eh"].toString().toULongLong(&eh,16);
        value.edgeHashV=object["ev"].toString().toULongLong(&ev,16);
        value.edgeAHash=object["ea"].toString().toULongLong(&ea,16);
        value.fgCellMask64=object["fm64"].toString().toULongLong(&fm,16);
        auto sbytes=QByteArray::fromBase64(object["sgrid"].toString().toLatin1());
        if(sh && sv && sa && eh && ev && ea && fm && sbytes.size()==48) {
            memcpy(value.subjRgbGrid,sbytes.constData(),48);
            value.subjFgMask=uint16_t(object["sfm"].toInt(0));
            value.bgCoherencePermille=uint16_t(std::clamp(object["bgc"].toInt(0),0,1000));
            value.hasSubjectDetail=true;
        }
    }
    value.hasCenterCrop=false;
    if(object["crop"].toBool()) {
        bool ch=false,cv=false,ca=false;
        value.centerDHashH=object["ch"].toString().toULongLong(&ch,16);
        value.centerDHashV=object["cv"].toString().toULongLong(&cv,16);
        value.centerAHash=object["ca"].toString().toULongLong(&ca,16);
        auto cbytes=QByteArray::fromBase64(object["cgrid"].toString().toLatin1());
        if(ch && cv && ca && cbytes.size()==48) {
            memcpy(value.centerRgbGrid,cbytes.constData(),48);
            bool csh=false,csv=false,csa=false,ceh=false,cev=false,cea=false,cfm=false;
            value.centerSubjDHashH=object["csh"].toString().toULongLong(&csh,16);
            value.centerSubjDHashV=object["csv"].toString().toULongLong(&csv,16);
            value.centerSubjAHash=object["csa"].toString().toULongLong(&csa,16);
            value.centerEdgeHashH=object["ceh"].toString().toULongLong(&ceh,16);
            value.centerEdgeHashV=object["cev"].toString().toULongLong(&cev,16);
            value.centerEdgeAHash=object["cea"].toString().toULongLong(&cea,16);
            value.centerFgCellMask64=object["cfm64"].toString().toULongLong(&cfm,16);
            auto csbytes=QByteArray::fromBase64(object["csgrid"].toString().toLatin1());
            if(csh && csv && csa && ceh && cev && cea && cfm && csbytes.size()==48) {
                memcpy(value.centerSubjRgbGrid,csbytes.constData(),48);
                value.centerSubjFgMask=uint16_t(object["csfm"].toInt(0));
                value.centerBgCoherencePermille=uint16_t(std::clamp(object["cbgc"].toInt(0),0,1000));
            }
            value.hasCenterCrop=true;
        }
    }
    return true;
}
bool isRawImageExtension(const QString& ext) {
    static const QSet<QString> kRawExts={
        "dng","cr2","cr3","nef","nrw","arw","srf","sr2","raf","orf",
        "rw2","pef","srw","erf","kdc","dcr","3fr","mef","mos","iiq","raw"
    };
    return kRawExts.contains(ext);
}
// Version 4 changed how transparency is flattened; version 5 fixed Camera RAW embedded preview & reduced-res IFD0 selection
// (and half-float EXR/PHM tone-mapping); version 6 records full Camera RAW sensor dimensions instead of embedded preview dimensions.
int imageRenderVersion(const std::wstring& path) {
    if(!VideoScanner::IsImageExtension(path)) return 0;
    auto extension=QFileInfo(QString::fromStdWString(path)).suffix().toLower();
    if(extension=="jpg" || extension=="jpeg" || extension=="jpe" || extension=="jfif") return 3;
    if(isRawImageExtension(extension)) return 6;
    if(extension=="exr" || extension=="phm") return 5;
    return 4;
}
QByteArray encode(const VideoFileInfo& file,bool thumbnail=true) {
    QJsonArray samples; for(const auto& sample:file.samples) samples.append(signature(sample));
    const int currentImgRenderVer=imageRenderVersion(file.fullPath);
    const int encodedImgRenderVer=(currentImgRenderVer>0 && file.cachedImageRenderVersion>0)?int(file.cachedImageRenderVersion):currentImgRenderVer;
    const int encodedDetailsVer=file.cachedDetailsVersion>0?int(file.cachedDetailsVersion):2;
    QJsonObject object{{"duration",QString::number(file.duration100ns)},{"width",int(file.width)},{"height",int(file.height)},
        {"renderVersion",2},{"imageRenderVersion",encodedImgRenderVer},{"imageDetailVersion",VideoScanner::IsImageExtension(file.fullPath)?1:0},{"detailsVersion",encodedDetailsVer},
        {"imageKind",int(file.imageKind)},{"imageFrames",int(file.imageFrameCount)},
        {"createdTime",QString::number(file.createdTime)},{"captureTime",QString::number(file.captureTimeSec)},
        {"videoBitrate",int(file.videoBitrate)},{"audioChannels",file.audioChannels},{"audioRate",int(file.audioSampleRate)},{"audioCodec",QString::fromStdWString(file.audioCodec)},{"fps",file.fps},{"codec",QString::fromStdWString(file.codec)},{"hash",QString::fromStdString(file.contentHash)},
        {"metadata",file.metadataLoaded},{"frame",file.frameLoaded},{"previewFailed",file.previewFailed},{"first",signature(file.sigFirst)},{"alternate",signature(file.sigAlt)},
        {"samples",samples},{"thumb",thumbnail?QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(file.thumbBGRA.data()),file.thumbBGRA.size()).toBase64()):QString()}};
    object.insert("evidenceVersion",3); if(thumbnail && !file.thumbBGRA.empty()) object.insert("thumbVersion",2);
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}
bool decode(const QByteArray& payload,VideoFileInfo& file,bool allowLegacyRenderVersion=false) {
    if(payload.size()>131072) return false;
    QByteArray raw=payload;
    if(payload.startsWith("CZ1")) {
        if(payload.size()<8) return false;
        const auto size=qFromBigEndian<quint32>(payload.constData()+3);
        if(!size || size>131072) return false;
        raw.resize(size); uLongf actual=size;
        if(uncompress(reinterpret_cast<Bytef*>(raw.data()),&actual,reinterpret_cast<const Bytef*>(payload.constData()+7),uLong(payload.size()-7))!=Z_OK || actual!=size) return false;
    }
    auto document=QJsonDocument::fromJson(raw); if(!document.isObject()) return false;
    auto object=document.object(); VideoFileInfo value=file;
    bool duration=false; value.duration100ns=object["duration"].toString().toLongLong(&duration);
    int width=object["width"].toInt(-1),height=object["height"].toInt(-1);
    const int dimensionLimit=VideoScanner::IsImageExtension(file.fullPath)?131072:16384;
    if(!duration || value.duration100ns<0 || width<0 || height<0 || width>dimensionLimit || height>dimensionLimit || uint64_t(width)*height>1073741824) return false;
    value.width=width; value.height=height; value.fps=object["fps"].toDouble();
    if(!std::isfinite(value.fps) || value.fps<0 || value.fps>1000) return false;
    value.codec=object["codec"].toString().toStdWString(); value.contentHash=object["hash"].toString().toStdString();
    if(!value.contentHash.empty() && (value.contentHash.size()!=64 || value.contentHash.find_first_not_of("0123456789abcdef")!=std::string::npos)) return false;
    if(object.contains("createdTime")) {
        bool cOk=false; const uint64_t ct=object["createdTime"].toString().toULongLong(&cOk);
        if(!cOk) return false;
        if(ct>0) value.createdTime=ct;
    }
    if(object.contains("captureTime")) {
        bool capOk=false; const int64_t cap=object["captureTime"].toString().toLongLong(&capOk);
        if(!capOk || cap<0 || cap>4102444800LL) return false;
        value.captureTimeSec=cap;
    }
    value.imageKind=static_cast<ImageEvidenceKind>(std::clamp(object["imageKind"].toInt(0),0,8));
    value.imageFrameCount=static_cast<uint32_t>(std::clamp(object["imageFrames"].toInt(1),1,100000));
    value.videoBitrate=uint32_t(std::max(0,object["videoBitrate"].toInt()));
    value.audioChannels=object["audioChannels"].toInt(-1); value.audioSampleRate=uint32_t(std::max(0,object["audioRate"].toInt())); value.audioCodec=object["audioCodec"].toString().toStdWString();
    if(value.audioChannels<-1 || value.audioChannels>128 || value.audioSampleRate>768000) return false;
    const bool isImage=VideoScanner::IsImageExtension(file.fullPath);
    const int detailsVer=object["detailsVersion"].toInt();
    value.cachedDetailsVersion=(!isImage && detailsVer==1)?2:((detailsVer>=1 && detailsVer<=255)?uint8_t(detailsVer):2);
    value.metadataLoaded=object["metadata"].toBool() && (detailsVer==2 || (!isImage && detailsVer==1) || (allowLegacyRenderVersion && detailsVer==1));
    value.frameLoaded=object["frame"].toBool();
    value.previewFailed=object["previewFailed"].toBool(value.previewFailed);
    if(!signature(object["first"].toObject(),value.sigFirst) || !signature(object["alternate"].toObject(),value.sigAlt)) return false;
    auto samples=object["samples"].toArray(); if(!samples.isEmpty() && (samples.size()<2 || samples.size()>5)) return false;
    value.samples.clear(); for(auto sample:samples) { FrameSignature s; if(!signature(sample.toObject(),s)) return false; value.samples.push_back(s); }
    auto thumbnail=QByteArray::fromBase64(object["thumb"].toString().toLatin1());
    if(!thumbnail.isEmpty() && thumbnail.size()!=THUMB_W*THUMB_H*4) return false;
    value.thumbBGRA.assign(thumbnail.begin(),thumbnail.end()); if(isImage && object["thumbVersion"].toInt()!=2) value.thumbBGRA.clear();
    // Rotation, HDR normalization, and RAW preview selection changed displayed pixels; keep SHA in cache while preserving frozen .cvsession snapshots.
    const int savedImgRenderVer=object["imageRenderVersion"].toInt();
    const int currentImgRenderVer=imageRenderVersion(file.fullPath);
    const bool imgRenderOk=!isImage ||
        savedImgRenderVer==currentImgRenderVer ||
        (allowLegacyRenderVersion && ((savedImgRenderVer==4 && currentImgRenderVer==5) || ((savedImgRenderVer==4 || savedImgRenderVer==5) && currentImgRenderVer==6)));
    value.cachedImageRenderVersion=(currentImgRenderVer>0 && savedImgRenderVer>0 && savedImgRenderVer<=255)?uint8_t(savedImgRenderVer):0;
    if(object["renderVersion"].toInt()!=2 || !imgRenderOk) {
        value.cachedImageRenderVersion=0; value.cachedDetailsVersion=2;
        value.duration100ns=0; value.width=0; value.height=0; value.fps=0.0;
        value.captureTimeSec=0;
        value.imageKind=ImageEvidenceKind::StaticImage; value.imageFrameCount=1;
        value.videoBitrate=0; value.audioChannels=-1; value.audioSampleRate=0;
        value.audioCodec.clear(); value.codec.clear();
        value.metadataLoaded=false; value.frameLoaded=false;
        value.previewFailed=false; if(!value.corrupted) value.error=S_OK;
        value.sigFirst={}; value.sigAlt={}; value.samples.clear(); value.thumbBGRA.clear();
    }
    // Crop and foreground subject signatures change matching evidence, not source metadata or SHA.
    if(object["evidenceVersion"].toInt()!=3 || (VideoScanner::IsImageExtension(file.fullPath) && object["imageDetailVersion"].toInt()!=1)) { value.frameLoaded=false; value.sigFirst={}; value.sigAlt={}; value.samples.clear(); value.thumbBGRA.clear(); }
    file=std::move(value); return true;
}
}
QByteArray ScanCache::encodeEvidence(const VideoFileInfo& file) { return encode(file,false); }
bool ScanCache::decodeEvidence(const QByteArray& payload,VideoFileInfo& file,bool allowLegacyRenderVersion) { return decode(payload,file,allowLegacyRenderVersion); }

ScanCache::ScanCache(const QString& path) : connection_(QUuid::createUuid().toString()) {
    database_=QSqlDatabase::addDatabase("QSQLITE",connection_); database_.setDatabaseName(path);
    database_.setConnectOptions("QSQLITE_BUSY_TIMEOUT=1500");
    if(!database_.open()) { error_=database_.lastError().text(); return; }
    QSqlQuery query(database_);
    // WAL lets the scan writer and UI readers proceed together; synchronous stays FULL so journal commits remain durable.
    query.exec("PRAGMA journal_mode=WAL"); query.finish();
    if(!query.exec("CREATE TABLE IF NOT EXISTS scan_cache_v4(path TEXT PRIMARY KEY, identity TEXT NOT NULL, payload BLOB NOT NULL)")) error_=query.lastError().text();
    if(error_.isEmpty() && !query.exec("CREATE TABLE IF NOT EXISTS media_cache_policy_v1(id INTEGER PRIMARY KEY CHECK(id=1),enabled INTEGER NOT NULL CHECK(enabled IN(0,1)))")) error_=query.lastError().text();
    if(error_.isEmpty()) {
        if(!query.exec("SELECT enabled FROM media_cache_policy_v1 WHERE id=1")) error_=query.lastError().text();
        else mediaCachingEnabled_=!query.next() || query.value(0).toBool();
        query.finish();
    }
    if(error_.isEmpty() && !query.exec("CREATE TABLE IF NOT EXISTS operations_v1(id INTEGER PRIMARY KEY, time TEXT NOT NULL, path TEXT NOT NULL, identity TEXT NOT NULL, result INTEGER NOT NULL)")) error_=query.lastError().text();
    if(error_.isEmpty() && !query.exec("CREATE TABLE IF NOT EXISTS ignored_pairs_v1(pair TEXT PRIMARY KEY)")) error_=query.lastError().text();
    if(error_.isEmpty() && !query.exec("CREATE TABLE IF NOT EXISTS review_decisions_v1(pair TEXT PRIMARY KEY, decision INTEGER NOT NULL CHECK(decision BETWEEN 1 AND 3))")) error_=query.lastError().text();
    if(error_.isEmpty() && !query.exec("CREATE TABLE IF NOT EXISTS transfers_v1("
        "id INTEGER PRIMARY KEY,time TEXT NOT NULL,source TEXT NOT NULL,destination TEXT NOT NULL,operation TEXT NOT NULL,"
        "source_identity TEXT NOT NULL,destination_identity TEXT NOT NULL DEFAULT '',hash TEXT NOT NULL DEFAULT '',"
        "copied INTEGER NOT NULL DEFAULT 0,moved INTEGER NOT NULL DEFAULT 0,phase TEXT NOT NULL DEFAULT 'pending',"
        "result INTEGER NOT NULL,undone INTEGER NOT NULL DEFAULT 0,undo_result INTEGER,restored_identity TEXT NOT NULL DEFAULT '')")) error_=query.lastError().text();
    const auto tables=database_.tables();
    legacyTable_=tables.contains("scan_cache_v3")?"scan_cache_v3":tables.contains("scan_cache_v2")?"scan_cache_v2":QString();
}
ScanCache::~ScanCache() { database_.close(); database_=QSqlDatabase(); QSqlDatabase::removeDatabase(connection_); }
void ScanCache::ignorePair(const VideoFileInfo& a,const VideoFileInfo& b) {
    auto pair=ReviewPairKey(a,b); if(!error_.isEmpty() || pair.empty()) return;
    QSqlQuery query(database_); query.prepare("INSERT OR IGNORE INTO ignored_pairs_v1(pair) VALUES(?)"); query.addBindValue(QString::fromStdString(pair));
    if(!query.exec()) error_=query.lastError().text();
}
std::set<std::string> ScanCache::ignoredPairs() {
    std::set<std::string> result; if(!error_.isEmpty()) return result;
    QSqlQuery query(database_); if(!query.exec("SELECT pair FROM ignored_pairs_v1")) { error_=query.lastError().text(); return result; }
    while(query.next()) result.insert(query.value(0).toString().toStdString()); return result;
}
ReviewDecisions ScanCache::reviewDecisions() {
    ReviewDecisions result; if(!error_.isEmpty()) return result;
    QSqlQuery query(database_); if(!query.exec("SELECT pair,decision FROM review_decisions_v1")) { error_=query.lastError().text(); return result; }
    while(query.next()) {
        auto pair=query.value(0).toString().toStdString(); const auto value=query.value(1).toInt();
        if(pair.empty() || pair.size()>220 || pair.find('/')==std::string::npos || value<1 || value>3) { error_="Invalid review decision"; return {}; }
        result.emplace(std::move(pair),static_cast<ReviewDecision>(value));
    }
    return result;
}
void ScanCache::saveReviewDecisions(const ReviewDecisions& decisions) {
    if(!error_.isEmpty() || decisions.empty()) return;
    if(!database_.transaction()) { error_=database_.lastError().text(); return; }
    QSqlQuery query(database_);
    for(const auto& [pair,decision]:decisions) {
        const auto value=int(decision);
        if(pair.empty() || pair.size()>220 || value<0 || value>3) { error_="Invalid review decision"; database_.rollback(); return; }
        query.prepare(value==0?"DELETE FROM review_decisions_v1 WHERE pair=?":"INSERT INTO review_decisions_v1(pair,decision) VALUES(?,?) ON CONFLICT(pair) DO UPDATE SET decision=excluded.decision");
        query.addBindValue(QString::fromStdString(pair)); if(value) query.addBindValue(value);
        if(!query.exec()) { error_=query.lastError().text(); database_.rollback(); return; }
    }
    if(!database_.commit()) error_=database_.lastError().text();
}
void ScanCache::clearReviewDecisions() {
    if(!error_.isEmpty()) return;
    QSqlQuery query(database_); if(!query.exec("DELETE FROM review_decisions_v1")) error_=query.lastError().text();
}
void ScanCache::clearIgnoredPairs() {
    if(!error_.isEmpty()) return;
    QSqlQuery query(database_); if(!query.exec("DELETE FROM ignored_pairs_v1")) error_=query.lastError().text();
}
void ScanCache::mergeIgnoredPairs(const std::set<std::string>& pairs) {
    if(!error_.isEmpty() || pairs.empty()) return;
    if(!database_.transaction()) { error_=database_.lastError().text(); return; }
    QSqlQuery query(database_); query.prepare("INSERT OR IGNORE INTO ignored_pairs_v1(pair) VALUES(?)");
    for(const auto& pair:pairs) {
        query.bindValue(0,QString::fromStdString(pair));
        if(!query.exec()) { error_=query.lastError().text(); database_.rollback(); return; }
    }
    if(!database_.commit()) error_=database_.lastError().text();
}
qint64 ScanCache::beginRemoval(const VideoFileInfo& file) {
    if(!error_.isEmpty()) return 0;
    QSqlQuery query(database_);
    query.prepare("INSERT INTO operations_v1(time,path,identity,result) VALUES(strftime('%Y-%m-%dT%H:%M:%fZ','now'),?,?,?)");
    query.addBindValue(QString::fromStdWString(file.fullPath)); query.addBindValue(identity(file)); query.addBindValue(qlonglong(E_PENDING));
    if(!query.exec()) { error_=query.lastError().text(); return 0; }
    return query.lastInsertId().toLongLong();
}
void ScanCache::completeRemoval(qint64 id,HRESULT result) {
    if(!error_.isEmpty() || id<=0) return;
    QSqlQuery query(database_); query.prepare("UPDATE operations_v1 SET result=? WHERE id=? AND result=?");
    query.addBindValue(qlonglong(result)); query.addBindValue(id); query.addBindValue(qlonglong(E_PENDING));
    if(!query.exec()) error_=query.lastError().text();
    else if(query.numRowsAffected()!=1) error_="File operation journal entry is not pending";
}
void ScanCache::recordRemoval(const VideoFileInfo& file,HRESULT result) { auto id=beginRemoval(file); completeRemoval(id,result); }
qint64 ScanCache::beginTransfer(const VideoFileInfo& file,const QString& destination,const QString& operation) {
    if(!error_.isEmpty()) return 0;
    const auto source=QString::fromStdWString(file.fullPath);
    if(!file.identity.valid || !QFileInfo(source).isAbsolute() || !QFileInfo(destination).isAbsolute() || source.contains(QChar(0)) || destination.contains(QChar(0)) || source.size()>32767 || destination.size()>32767 ||
       (operation!="copy" && operation!="move" && operation!="quarantine")) { error_="Invalid transfer plan"; return 0; }
    QSqlQuery query(database_);
    query.prepare("INSERT INTO transfers_v1(time,source,destination,operation,source_identity,result) VALUES(strftime('%Y-%m-%dT%H:%M:%fZ','now'),?,?,?,?,?)");
    query.addBindValue(source); query.addBindValue(destination); query.addBindValue(operation); query.addBindValue(identity(file)); query.addBindValue(qlonglong(E_PENDING));
    if(!query.exec()) { error_=query.lastError().text(); return 0; } return query.lastInsertId().toLongLong();
}
void ScanCache::completeTransfer(qint64 id,const FileOperations::TransferResult& result) {
    if(!error_.isEmpty() || id<=0) return;
    QSqlQuery query(database_); query.prepare("UPDATE transfers_v1 SET destination_identity=?,hash=?,copied=?,moved=?,phase='finished',result=? WHERE id=? AND phase='pending'");
    query.addBindValue(identity(result.destinationIdentity)); query.addBindValue(QString::fromStdString(result.contentHash)); query.addBindValue(result.copied); query.addBindValue(result.moved);
    query.addBindValue(qlonglong(result.error)); query.addBindValue(id);
    if(!query.exec()) error_=query.lastError().text(); else if(query.numRowsAffected()!=1) error_="Transfer journal entry is not pending";
}
std::optional<TransferRecord> ScanCache::lastUndoableTransfer() {
    if(!error_.isEmpty()) return {};
    QSqlQuery query(database_);
    if(!query.exec("SELECT id,source,destination,operation,source_identity,destination_identity,hash,copied,moved,phase,result,undone,restored_identity FROM transfers_v1 WHERE undone=0 AND (copied=1 OR phase!='finished') ORDER BY id DESC LIMIT 1")) { error_=query.lastError().text(); return {}; }
    if(!query.next()) return {};
    TransferRecord record; record.id=query.value(0).toLongLong(); record.source=query.value(1).toString(); record.destination=query.value(2).toString(); record.operation=query.value(3).toString();
    record.contentHash=query.value(6).toString().toStdString(); record.copied=query.value(7).toBool(); record.moved=query.value(8).toBool(); record.phase=query.value(9).toString(); record.result=HRESULT(query.value(10).toLongLong()); record.undone=query.value(11).toBool();
    if(record.phase!="finished") return {}; // Interrupted operations require review; never infer ownership from paths.
    if(!parseIdentity(query.value(4).toString(),record.sourceIdentity) || !parseIdentity(query.value(5).toString(),record.destinationIdentity) || !parseIdentity(query.value(12).toString(),record.restoredIdentity) ||
       !record.sourceIdentity.valid || !record.destinationIdentity.valid || !QFileInfo(record.source).isAbsolute() || !QFileInfo(record.destination).isAbsolute() ||
       record.source.contains(QChar(0)) || record.destination.contains(QChar(0)) || record.source.size()>32767 || record.destination.size()>32767 ||
       (record.operation!="copy" && record.operation!="move" && record.operation!="quarantine") || record.contentHash.size()!=64 || record.contentHash.find_first_not_of("0123456789abcdef")!=std::string::npos) { error_="Invalid transfer journal record"; return {}; }
    return record;
}
bool ScanCache::beginTransferUndo(qint64 id) {
    const auto record=lastUndoableTransfer(); if(!record || record->id!=id) return false;
    QSqlQuery query(database_); query.prepare("UPDATE transfers_v1 SET phase='undo_pending',undo_result=? WHERE id=? AND phase='finished' AND undone=0"); query.addBindValue(qlonglong(E_PENDING)); query.addBindValue(id);
    if(!query.exec()) { error_=query.lastError().text(); return false; } return query.numRowsAffected()==1;
}
void ScanCache::completeTransferUndo(qint64 id,const FileOperations::TransferResult& result) {
    if(!error_.isEmpty() || id<=0) return;
    QSqlQuery query(database_); query.prepare("UPDATE transfers_v1 SET phase='finished',undo_result=?,undone=?,restored_identity=CASE WHEN ?<>'' THEN ? ELSE restored_identity END WHERE id=? AND phase='undo_pending'");
    const auto restored=identity(result.destinationIdentity);
    query.addBindValue(qlonglong(result.error)); query.addBindValue(result.succeeded()); query.addBindValue(restored); query.addBindValue(restored); query.addBindValue(id);
    if(!query.exec()) error_=query.lastError().text(); else if(query.numRowsAffected()!=1) error_="Transfer undo journal entry is not pending";
}
std::vector<OperationEntry> ScanCache::operationEntries() {
    std::vector<OperationEntry> entries; if(!error_.isEmpty()) return entries;
    QSqlQuery query(database_);
    if(!query.exec("SELECT time,path,result,'Recycle Bin' AS operation,'' AS destination,'finished' AS phase,0 AS undone,NULL AS undo_result,0 AS copied FROM operations_v1 "
        "UNION ALL SELECT time,source,result,operation,destination,phase,undone,undo_result,copied FROM transfers_v1 ORDER BY time DESC LIMIT 1000")) { error_=query.lastError().text(); return entries; }
    auto status=[](HRESULT result) { return SUCCEEDED(result)?Language::text("Completed"):Language::text("Failed")+" · HRESULT 0x"+QString::number(quint32(result),16).rightJustified(8,'0'); };
    while(query.next()) {
        OperationEntry entry; auto result=HRESULT(query.value(2).toLongLong());
        const bool interrupted=result==E_PENDING || query.value(5).toString()!="finished";
        entry.attention=interrupted || FAILED(result);
        entry.outcome=interrupted?Language::text("Outcome unconfirmed after interruption; rescan before retry"):status(result);
        if(query.value(6).toBool()) entry.outcome=Language::text("Undone")+" · "+entry.outcome;
        else if(FAILED(result) && query.value(8).toBool()) entry.outcome=Language::text("Copy phase completed; inspect source and destination")+" · "+entry.outcome;
        if(!query.value(7).isNull()) entry.outcome+=" · "+Language::text("Undo")+": "+status(HRESULT(query.value(7).toLongLong()));
        entry.time=query.value(0).toString(); entry.operation=Language::text(query.value(3).toString().toUtf8().constData());
        entry.path=query.value(1).toString(); entry.destination=query.value(4).toString();
        entries.push_back(std::move(entry));
    }
    return entries;
}
QStringList ScanCache::operationHistory() {
    QStringList rows;
    for(const auto& entry:operationEntries()) rows<<entry.time+"  ·  "+entry.operation+"  ·  "+entry.outcome+"\n"+entry.path+(entry.destination.isEmpty()?QString():"\n→ "+entry.destination);
    return rows;
}
bool ScanCache::restore(VideoFileInfo& file) {
    if(!error_.isEmpty() || !mediaCachingEnabled_ || !file.identity.valid) return false;
    QSqlQuery query(database_); query.prepare("SELECT payload FROM scan_cache_v4 WHERE path=? AND identity=?"); query.addBindValue(key(file)); query.addBindValue(identity(file));
    if(!query.exec()) { error_=query.lastError().text(); return false; }
    if(query.next() && decode(query.value(0).toByteArray(),file)) return true;
    if(legacyTable_.isEmpty()) return false;
    query.prepare(QString("SELECT payload FROM %1 WHERE path=? AND identity=?").arg(legacyTable_));
    query.addBindValue(key(file)); query.addBindValue(identity(file));
    if(!query.exec()) { error_=query.lastError().text(); return false; }
    if(!query.next() || !decode(query.value(0).toByteArray(),file)) return false;
    // Identity-valid metadata and SHA remain useful; old color conversion evidence must be regenerated.
    file.frameLoaded=false; file.previewFailed=false; if(!file.corrupted) file.error=S_OK; file.sigFirst={}; file.sigAlt={}; file.samples.clear(); file.thumbBGRA.clear();
    return true;
}
unsigned ScanCache::restoreBatch(std::vector<VideoFileInfo>& files,const std::atomic<bool>& cancel) {
    if(!error_.isEmpty() || !mediaCachingEnabled_ || files.empty()) return 0;
    QSqlQuery primary(database_);
    if(!primary.prepare("SELECT payload FROM scan_cache_v4 WHERE path=? AND identity=?")) { error_=primary.lastError().text(); return 0; }
    QSqlQuery legacy(database_);
    const bool hasLegacy=!legacyTable_.isEmpty();
    if(hasLegacy && !legacy.prepare(QString("SELECT payload FROM %1 WHERE path=? AND identity=?").arg(legacyTable_))) { error_=legacy.lastError().text(); return 0; }
    unsigned hits=0;
    for(auto& file:files) {
        if(cancel.load(std::memory_order_relaxed)) break;
        if(file.deleted || !file.identity.valid) continue;
        const auto k=key(file); const auto id=identity(file);
        primary.bindValue(0,k); primary.bindValue(1,id);
        if(!primary.exec()) { error_=primary.lastError().text(); break; }
        const bool okPrimary=primary.next() && decode(primary.value(0).toByteArray(),file);
        primary.finish();
        if(okPrimary) { ++hits; continue; }
        if(!hasLegacy) continue;
        legacy.bindValue(0,k); legacy.bindValue(1,id);
        if(!legacy.exec()) { error_=legacy.lastError().text(); break; }
        const bool okLegacy=legacy.next() && decode(legacy.value(0).toByteArray(),file);
        legacy.finish();
        if(!okLegacy) continue;
        file.frameLoaded=false; file.previewFailed=false; if(!file.corrupted) file.error=S_OK; file.sigFirst={}; file.sigAlt={}; file.samples.clear(); file.thumbBGRA.clear();
        ++hits;
    }
    return hits;
}
bool ScanCache::exportOperationHistory(const QString& path) {
    if(!error_.isEmpty()) return false;
    QSqlQuery query(database_);
    if(!query.exec("SELECT id,time,path,identity,result,'recycle_bin' AS operation,'' AS destination,'finished' AS phase,0 AS undone,NULL AS undo_result,0 AS copied,0 AS moved FROM operations_v1 "
        "UNION ALL SELECT id,time,source,source_identity,result,operation,destination,phase,undone,undo_result,copied,moved FROM transfers_v1 ORDER BY time,id")) { error_=query.lastError().text(); return false; }
    QSaveFile output(path); if(!output.open(QIODevice::WriteOnly)) return false;
    const auto header=QByteArray::fromHex("efbbbf")+"id,time_utc,path,identity,operation,outcome,hresult,destination,phase,undo_hresult,copied,moved\r\n";
    if(output.write(header)!=header.size()) return false;
    while(query.next()) {
        auto result=HRESULT(query.value(4).toLongLong());
        const QString outcome=query.value(8).toBool()?"undone":result==E_PENDING || query.value(7).toString()!="finished"?"unconfirmed":SUCCEEDED(result)?"succeeded":query.value(10).toBool()?"partial":"failed";
        QStringList row{query.value(0).toString(),query.value(1).toString(),query.value(2).toString(),query.value(3).toString(),query.value(5).toString(),outcome,"0x"+QString::number(quint32(result),16).rightJustified(8,'0'),
            query.value(6).toString(),query.value(7).toString(),query.value(9).isNull()?QString():"0x"+QString::number(quint32(query.value(9).toLongLong()),16).rightJustified(8,'0'),query.value(10).toString(),query.value(11).toString()};
        for(auto& value:row) value=Csv::field(value);
        const auto bytes=row.join(',').toUtf8()+"\r\n";
        if(output.write(bytes)!=bytes.size()) return false;
    }
    if(query.lastError().isValid()) { error_=query.lastError().text(); return false; }
    return output.commit();
}
void ScanCache::store(const std::vector<VideoFileInfo>& files,bool pruneNow) {
    if(!error_.isEmpty() || !mediaCachingEnabled_) return;
    if(!database_.transaction()) { error_=database_.lastError().text(); return; }
    QSqlQuery query(database_); query.prepare("INSERT OR REPLACE INTO scan_cache_v4(path,identity,payload) VALUES(?,?,?)");
    for(const auto& file:files) {
        const bool cacheableError=SUCCEEDED(file.error) || (file.previewFailed && (!file.contentHash.empty() || file.metadataLoaded));
        if(!file.identity.valid || file.deleted || file.corrupted || !cacheableError || (!file.metadataLoaded && file.contentHash.empty())) continue;
        if((file.metadataLoaded && file.cachedDetailsVersion!=2) ||
           (file.cachedImageRenderVersion>0 && int(file.cachedImageRenderVersion)!=imageRenderVersion(file.fullPath))) continue;
        // Keep comparison evidence for large archives; visible thumbnails are decoded lazily.
        const auto evidence=encode(file,false); if(evidence.size()>131072) continue;
        query.bindValue(0,key(file)); query.bindValue(1,identity(file)); query.bindValue(2,QByteArray("CZ1")+qCompress(evidence,1));
        if(!query.exec()) { error_=query.lastError().text(); database_.rollback(); return; }
    }
    if(!database_.commit()) { error_=database_.lastError().text(); return; }
    if(pruneNow) prune();
}
void ScanCache::prune() {
    if(!error_.isEmpty() || !mediaCachingEnabled_) return;
    if(!database_.transaction()) { error_=database_.lastError().text(); return; }
    QSqlQuery query(database_);
    // Review decisions and history are separate from this bounded decode cache.
    query.prepare("DELETE FROM scan_cache_v4 WHERE rowid IN (SELECT rowid FROM ("
        "SELECT rowid,ROW_NUMBER() OVER (ORDER BY rowid DESC) AS position,"
        "SUM(LENGTH(payload)) OVER (ORDER BY rowid DESC ROWS UNBOUNDED PRECEDING) AS bytes "
        "FROM scan_cache_v4) WHERE position>? OR bytes>?)");
    query.addBindValue(MaxMediaEntries);
    query.addBindValue(MaxMediaPayloadBytes);
    if(!query.exec()) { error_=query.lastError().text(); database_.rollback(); return; }
    if(!database_.commit()) error_=database_.lastError().text();
}

// Interrupted entries stay: they are the only record of an operation whose outcome still needs review.
void ScanCache::clearOperationHistory() {
    if(!error_.isEmpty()) return;
    QSqlQuery query(database_);
    query.prepare("DELETE FROM operations_v1 WHERE result<>?"); query.addBindValue(qlonglong(E_PENDING));
    if(!query.exec()) { error_=query.lastError().text(); return; }
    query.prepare("DELETE FROM transfers_v1 WHERE phase='finished' AND result<>? AND (undo_result IS NULL OR undo_result<>?)"); query.addBindValue(qlonglong(E_PENDING)); query.addBindValue(qlonglong(E_PENDING));
    if(!query.exec()) error_=query.lastError().text();
}
void ScanCache::clearScanData() {
    if(!error_.isEmpty()) return;
    QSqlQuery query(database_);
    if(!query.exec("DELETE FROM scan_cache_v4") || !query.exec("DROP TABLE IF EXISTS scan_cache_v3") || !query.exec("DROP TABLE IF EXISTS scan_cache_v2") || !query.exec("VACUUM")) error_=query.lastError().text();
    if(error_.isEmpty()) legacyTable_.clear();
    if(error_.isEmpty() && QFile::exists(database_.databaseName()+".scan-job") && !QFile::remove(database_.databaseName()+".scan-job")) error_="The saved scan list could not be cleared.";
}
void ScanCache::setMediaCachingEnabled(bool enabled) {
    if(!error_.isEmpty()) return;
    if(!database_.transaction()) { error_=database_.lastError().text(); return; }
    QSqlQuery query(database_);
    query.prepare("INSERT OR REPLACE INTO media_cache_policy_v1(id,enabled) VALUES(1,?)"); query.addBindValue(enabled);
    if(!query.exec() || (!enabled && (!query.exec("DELETE FROM scan_cache_v4") || !query.exec("DROP TABLE IF EXISTS scan_cache_v3") || !query.exec("DROP TABLE IF EXISTS scan_cache_v2")))) {
        error_=query.lastError().text(); database_.rollback(); return;
    }
    if(!database_.commit()) { error_=database_.lastError().text(); database_.rollback(); return; }
    mediaCachingEnabled_=enabled;
    if(!enabled) clearScanData();
}

CacheWriter::CacheWriter(QString path) : thread_([this,path=std::move(path)] {
    try {
        ScanCache cache(path);
        for(;;) {
            std::vector<VideoFileInfo> batch;
            {
                std::unique_lock lock(mutex_);
                changed_.wait(lock,[this] { return finished_ || !queue_.empty(); });
                if(queue_.empty() && finished_) break;
                while(!queue_.empty() && batch.size()<32) { batch.push_back(std::move(queue_.front())); queue_.pop_front(); }
            }
            changed_.notify_all();
            cache.store(batch,false);
            if(!cache.error().isEmpty()) {
                std::lock_guard lock(mutex_); error_=cache.error(); queue_.clear(); changed_.notify_all(); break;
            }
        }
        if(cache.error().isEmpty()) {
            cache.prune();
            if(!cache.error().isEmpty()) { std::lock_guard lock(mutex_); error_=cache.error(); }
        }
    } catch(...) {
        std::lock_guard lock(mutex_); error_="Cache writer failed"; queue_.clear(); changed_.notify_all();
    }
}) {}
CacheWriter::~CacheWriter() { finish(); }
void CacheWriter::submit(const VideoFileInfo& file) {
    const bool cacheableError=SUCCEEDED(file.error) || (file.previewFailed && (!file.contentHash.empty() || file.metadataLoaded));
    if(file.deleted || file.corrupted || !cacheableError || (!file.metadataLoaded && file.contentHash.empty())) return;
    std::unique_lock lock(mutex_);
    changed_.wait(lock,[this] { return queue_.size()<128 || finished_ || !error_.isEmpty(); });
    if(finished_ || !error_.isEmpty()) return;
    try { queue_.push_back(file); }
    catch(...) { error_="Cache queue allocation failed"; queue_.clear(); }
    lock.unlock(); changed_.notify_all();
}
void CacheWriter::finish() {
    { std::lock_guard lock(mutex_); finished_=true; }
    changed_.notify_all(); if(thread_.joinable()) thread_.join();
}
QString CacheWriter::error() { std::lock_guard lock(mutex_); return error_; }
