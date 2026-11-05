#pragma once
#include "Common.h"
#include "FileOperations.h"
#include <QSqlDatabase>
#include <QStringList>
#include <condition_variable>
#include <deque>
#include <optional>

struct TransferRecord {
    qint64 id=0;
    QString source,destination,operation,phase;
    FileIdentity sourceIdentity,destinationIdentity,restoredIdentity;
    std::string contentHash;
    bool copied=false,moved=false,undone=false;
    HRESULT result=E_PENDING;
};

// Each instance belongs to the thread that opens it.
// One journal row for display; time is the stored UTC ISO timestamp.
struct OperationEntry { QString time, operation, outcome, path, destination; bool attention=false; };

class ScanCache {
public:
    static constexpr qint64 MaxMediaPayloadBytes=256LL*1024*1024;
    static constexpr int MaxMediaEntries=100000;
    static QByteArray encodeEvidence(const VideoFileInfo& file);
    static bool decodeEvidence(const QByteArray& payload,VideoFileInfo& file);
    explicit ScanCache(const QString& path);
    ~ScanCache();
    bool restore(VideoFileInfo& file);
    // Pruning scans the whole table; batched writers defer it to prune() at the end.
    void store(const std::vector<VideoFileInfo>& files,bool pruneNow=true);
    void prune();
    qint64 beginRemoval(const VideoFileInfo& file);
    void completeRemoval(qint64 id,HRESULT result);
    void recordRemoval(const VideoFileInfo& file,HRESULT result);
    qint64 beginTransfer(const VideoFileInfo& source,const QString& destination,const QString& operation);
    void completeTransfer(qint64 id,const FileOperations::TransferResult& result);
    std::optional<TransferRecord> lastUndoableTransfer();
    bool beginTransferUndo(qint64 id);
    void completeTransferUndo(qint64 id,const FileOperations::TransferResult& result);
    std::vector<OperationEntry> operationEntries();
    QStringList operationHistory();
    bool exportOperationHistory(const QString& path);
    void clearOperationHistory();
    void ignorePair(const VideoFileInfo& a,const VideoFileInfo& b);
    std::set<std::string> ignoredPairs();
    void clearIgnoredPairs();
    void mergeIgnoredPairs(const std::set<std::string>& pairs);
    ReviewDecisions reviewDecisions();
    void saveReviewDecisions(const ReviewDecisions& decisions);
    void clearReviewDecisions();
    void clearScanData();
    bool mediaCachingEnabled() const { return mediaCachingEnabled_; }
    void setMediaCachingEnabled(bool enabled);
    QString error() const { return error_; }
private:
    QSqlDatabase database_;
    QString connection_, error_, legacyTable_;
    bool mediaCachingEnabled_=false;
};

class CacheWriter {
public:
    explicit CacheWriter(QString path);
    ~CacheWriter();
    void submit(const VideoFileInfo& file);
    void finish();
    QString error();
private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<VideoFileInfo> queue_;
    bool finished_=false;
    QString error_;
    std::thread thread_;
};
