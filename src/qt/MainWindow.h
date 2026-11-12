#pragma once
#include "VideoScanner.h"
#include "VideoPlayer.h"
#include "AudioEvidence.h"
#include "PartialClipSearch.h"
#include "ImageReader.h"
#include "ElidedLineEdit.h"
#include <QMainWindow>
#include <QPointF>
#include <QTimer>
#include <QTreeWidget>
#include <QStyleOptionViewItem>
#include <QSettings>
#include <QKeySequence>
#include <QSet>
#include <QThreadPool>
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <optional>

// Which file of each group is the reference (left, and kept by default). Values are stored in settings and sessions.
enum class ReferenceRule { HighestResolution=0, LargestFile=1, LongestDuration=2, OldestFile=3, NewestFile=4, NameOrder=5, OldestCreated=6, OldestCaptureDate=7 };

class QLabel;
class QSlider;
class QToolButton;
class QComboBox;
class QLineEdit;
class QProgressBar;
class QSplitter;
class QAction;
class QDoubleSpinBox;
class QSpinBox;
class QNetworkAccessManager;
class QMenu;
struct TransferRecord;

class VideoCanvas : public QWidget {
public:
    explicit VideoCanvas(VideoPlayer* player, QWidget* parent = nullptr);
    void setTransform(double zoom, QPointF pan);
    double zoom() const { return zoom_; }
    QPointF pan() const { return pan_; }
    int comparisonMode() const { return mode_; }
    void setComparison(VideoCanvas* reference,int mode) { reference_=reference; mode_=mode; update(); }
    void keyPressEvent(QKeyEvent*) override;
    void keyReleaseEvent(QKeyEvent*) override;
    void focusOutEvent(QFocusEvent*) override;
    void setPlayer(VideoPlayer* player) { player_=player; update(); }
    void setStillFrame(std::shared_ptr<const VideoFrame> frame,bool enabled,bool failed=false) { showingReference_=false; stillFrame_=std::move(frame); still_=enabled; stillFailed_=failed; update(); }
    std::shared_ptr<const VideoFrame> frame() const { return still_?stillFrame_:player_->GetFrame(); }
    void setPreviewHidden(bool hidden) { previewHidden_=hidden; repaint(); }
    std::function<void(double, QPointF)> transformChanged;
    std::function<bool(bool)> animationHold;
protected:
    void paintEvent(QPaintEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
private:
    VideoPlayer* player_;
    VideoCanvas* reference_=nullptr;
    int mode_=0;
    double wipe_=0.5;
    double zoom_ = 1;
    bool previewHidden_=false;
    bool showingReference_=false;
    bool animationHeld_=false;
    bool still_=false;
    bool stillFailed_=false;
    std::shared_ptr<const VideoFrame> stillFrame_;
    QPointF pan_, dragStart_;
};

class PlayerPanel : public QWidget {
public:
    void updateMetadata();
    explicit PlayerPanel(bool reference,QThreadPool* imagePool,QWidget* parent = nullptr);
    std::shared_ptr<VideoPlayer> player=std::make_shared<VideoPlayer>();
    std::shared_ptr<VideoPlayer> detachPlayer();
    VideoCanvas* canvas;
    int fileIndex = -1;
    std::function<void()> removeRequested, protectRequested, detailsRequested;
    std::function<void(QPoint)> fileMenuRequested;
    std::function<void(int64_t,bool)> seekRequested;
    std::function<void()> playRequested;
    std::function<void(bool)> stepRequested;
    std::function<void(int)> imageSeekRequested;
    std::function<void(bool)> imageStepRequested;
    bool isScrubbing() const { return seek_->isSliderDown(); }
    void setFile(const VideoFileInfo* file, int index);
    void setCandidates(const VideoGroup* group);
    void refresh();
    void setRemovalEnabled(bool enabled,const QString& explanation={});
    // The other file of the pair; differing metadata values are highlighted against it.
    void setPeer(const VideoFileInfo* other);
    void setMediaMode(bool images) { still_=images; transport_->setVisible(!images); }
    int loadedAnimationFrames() const { return int(std::min(imageSequence_.playbackFrames.size(),imageSequence_.frameDurations.size())); }
    bool animationReady() const { return imageSequence_.kind==ImageEvidenceKind::AnimatedImage && loadedAnimationFrames()>1; }
    bool imagePlaying() const { return imagePlaying_; }
    bool imageReady() const { return !still_ || bool(canvas->frame()); }
    bool imageDocument() const { return imageSequence_.kind==ImageEvidenceKind::MultiPageDocument; }
    void playImage(bool playing);
    void seekImage(int64_t time);
    void stepImage(bool forward);
    void tickImage();
    void showImageIndex(int index);
    void setImagePrivacy(bool hidden) { imageHidden_=hidden; if(hidden) playImage(false); }
    int imageFrameIndex() const { return imageFrameIndex_; }
    int64_t imageTime() const { return imageTime_; }
    int64_t imageTimeline() const { return imageTimeline_; }
    int64_t imageDuration() const { return imageSequence_.totalDuration100ns; }
    void translate();
private:
    bool reference_;
    QThreadPool* imagePool_;
    uint64_t imageRevision_=0;
    bool still_=false;
    StillImages::ImageExtendedInfo imageSequence_;
    VideoFileInfo imageSource_;
    bool imagePlaying_=false,imageHidden_=false;
    int imageFrameIndex_=0;
    int64_t imageTime_=0,imageTimeline_=0;
    QElapsedTimer imageClock_;
    QWidget* transport_;
    int activeRightSlot_ = 1, activeGroupTotal_ = 0;
    struct PeerInfo { bool valid=false,metadataLoaded=false,captureKnown=false; uint64_t size=0; int64_t duration=0; uint32_t width=0,height=0; int64_t captureTimeSec=0; std::wstring codec; };
    PeerInfo self_, peer_;
    QString path_;
    QLabel *role_, *folderTitle_, *metadata_, *time_, *frameCount_;
    void updateRole();
    ElidedLineEdit *name_, *pathLabel_;
    QSlider* seek_;
    QToolButton *play_, *mute_, *protect_, *remove_, *location_, *previous_, *next_, *details_;
    std::shared_ptr<const VideoFrame> shownFrame_; // last video frame the canvas was asked to paint
};

class MainWindow : public QMainWindow {
    friend class ResultsDelegate;
public:
    explicit MainWindow(const QString& dataDirectory = {}, QWidget* parent = nullptr);
    ~MainWindow() override;
    void openFiles(const QStringList& selection);
    bool openPaths(const QStringList& paths,bool addToSources=false);
    // Asks once, then looks for a newer release on GitHub at most daily (Updates.cpp); only for normal launches.
    void startUpdateChecks();
    // Empties the comparison and the result list and returns to the welcome card (File > Close results, new scans).
    void clearResults();
    // One place to remove what Compareel keeps about past scans, reviews and file operations (File > Clear history…).
    void showClearHistory();
    bool runUiCheck(const QString& mediaPath, const QString& screenshotPath);
    bool runResultsCheck(const QString& mediaPath, const QString& screenshotPath);
    bool runMatchingCheck(const QString& mediaPath, const QString& screenshotPath);
    bool runSessionScaleCheck();
    bool runLatencyCheck(const QString& mediaPath);
    bool runAppearanceCheck(const QString& mediaPath, const QString& screenshotPath);
    bool runSessionSnapshotCheck();
    bool runTransportCheck(const QString& mediaPath);
    bool runCriteriaCheck();
    bool runPathCheck(const QString& mediaPath);
    bool runCleanupRecoveryCheck(const QString& mediaPath);
    bool runReviewDecisionsCheck(const QString& mediaPath);
    bool runTransferCheck(const QString& mediaPath);
    bool runImageCheck();
    bool runOpenPairCheck(const QStringList& paths);
    bool runCachePolicyCheck();
    bool runEvidenceCheck();
    bool runHeifUiCheck(const QStringList& sources);
    bool runAudioEvidenceUiCheck(const QStringList& sources);
    bool runPartialSearchUiCheck(const QStringList& sources);
    bool runImageGuardCheck(const QStringList& sources);
    bool runBatchTransferCheck();
    bool runReviewResumeCheck(const QString& mediaPath);
    void runReviewResumeChild(const QString& mediaPath);
    void runCleanupCrashChild(const QString& mediaPath);
    bool runBrandingCheck(const QString& screenshotPath);
    bool runContinuationCheck(const QString& mediaPath);
    bool runFeedbackCheck(const QString& mediaPath, const QString& screenshotPath);
    bool runControlsCleanupCheck(const QString& mediaPath, const QString& screenshotPath);
    bool runTimingCheck(const QString& mediaPath, const QString& screenshotPath);
    void runCrashScanChild(const QString& folder,bool freezeList=false);
    bool runPlaybackCheck(const QString& mediaPath,const QString& screenshotPath,int durationSeconds=60);
    bool runSubtitleUiCheck(const QString& mediaPath,const QString& screenshotPath);
    bool runReviewToolsCheck(const QString& mediaPath,const QString& screenshotPath);
protected:
    bool eventFilter(QObject*,QEvent*) override;
    void closeEvent(QCloseEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dropEvent(QDropEvent*) override;
private:
    int proofLevel(const VideoGroup& group) const;
    void verifyResultHashes();
    void sortVerifiedGroups();
    QFutureWatcher<std::vector<VideoFileInfo>>* verificationWatcher_=nullptr;
    QFutureWatcher<AudioEvidence::Result>* audioEvidenceWatcher_=nullptr;
    void comparePairAudio();
    QFutureWatcher<PartialClipSearch::Result>* partialSearchWatcher_=nullptr;
    void findPartialClips();
    QComboBox* evidenceFilter_=nullptr;
    struct ScanResult { std::vector<VideoFileInfo> files; std::vector<VideoGroup> groups; std::vector<ScanIssue> issues; std::set<std::string> ignoredPairs; ScanMetrics metrics; QString error; QString cacheError; unsigned cacheHits=0,changedFiles=0; bool cancelled = false,manual=false,restored=false; };
    void createActions();
    void createLayout();
    void translate();
    void chooseFolders();
    void advancedSettings();
    void showSettings();
    void setTheme(const QString& id);
    void startScan(bool resume=false);
    void receiveScan(std::shared_ptr<ScanResult> result, uint64_t generation);
    void finishScanTiming(const QString& outcome);
    void updateScanTimeLabel();
    void fitToScreen();
    void populateResults();
    void finishResultsUpdate(int scroll);
    void replaceGroupRows(int groupIndex,int insertedGroups);
    void renumberGroupRows(int first);
    void updateSpaceSummary();
    void updateGroupRow(int groupIndex);
    // Result rows are painted by ResultsDelegate rather than built from widgets, so only visible rows cost anything.
    // These describe a row's thumbnails in viewport coordinates, including the row's own horizontal scroll.
    struct ThumbnailView { int fileIndex=-1; bool selected=false,previewHidden=false,hasImage=false; QString caption; QStringList badges; };
    QTreeWidgetItem* groupItem(int group) const;
    QRect thumbnailRect(int group,int slot) const;
    int thumbnailAt(int group,QPoint viewportPosition) const; // slot, -1 none, -2 left scroller, -3 right scroller
    int rowScroll(int group) const;
    int rowScrollMax(int group) const;
    void setRowScroll(int group,int pixels);
    void ensureThumbnailVisible(int group,int slot);
    ThumbnailView thumbnailView(int group,int slot) const;
    bool clickThumbnail(int group,int slot,Qt::MouseButton button=Qt::LeftButton); // self-checks: real mouse or context-menu events
    QStringList groupDetails(int group,int* proof=nullptr) const;
    void paintGroupRow(QPainter* painter,const QStyleOptionViewItem& option,int group) const;
    QString thumbnailToolTip(int group,int slot) const;
    void showThumbnailMenu(int group,int slot,QPoint global);
    void sortResults();
    void requestVisiblePreviews();
    void selectGroup(int group, int comparison = -1);
    void requestPreview(int index);
    void applyPrivacy();
    void updateSimilarity();
    void setVerdict(const QString& value,const char* kind);
    // A partial-clip alignment verdict stays visible for its pair until another pair is selected.
    QString partialVerdict_; const char* partialVerdictKind_="similar";
    std::pair<int,int> partialPair_{-1,-1};
    void updatePeers();
    void updateEmptyState();
    void syncEmptyStateChoices();
    void updateSourceSummary();
    void deleteFocusedFile();
    void advanceToNextPair();
    void toggleRightMark();
    QWidget* emptyState_=nullptr;
    struct { QToolButton* resetAlignment=nullptr; QToolButton* autoAlign=nullptr; QAction* autoAlignAction=nullptr; QAction* compareAudio=nullptr; QAction* markRight=nullptr; bool resolved=false; } tickTargets_;
    QComboBox* emptyMedia_=nullptr; QComboBox* emptyProfile_=nullptr;
    QWidget* reviewBar_=nullptr;
    QToolButton* markRightButton_=nullptr;
    QToolButton* cleanupMarkedButton_=nullptr;
    QNetworkAccessManager* network_=nullptr;
    bool updateChecksAvailable() const;
    void checkForUpdates(bool manual);
    struct CleanupTarget { int index; VideoFileInfo file, keeper; };
    enum class RetentionAttribute {
        CurrentSortOrder = 0,
        ResolutionHighest,
        DurationLongest,
        FpsHighest,
        SizeLargest,
        SizeSmallest,
        ModifiedNewest,
        ModifiedOldest,
        CreatedOldest,
        CaptureOldest
    };
    struct CleanupRetentionRule {
        RetentionAttribute attribute = RetentionAttribute::CurrentSortOrder;
        std::wstring preferredFolder;
    };
    int selectGroupKeeperSlot(const VideoGroup& group, const CleanupRetentionRule& rule) const;
    int selectGroupKeeperSlot(const VideoGroup& group) const { return selectGroupKeeperSlot(group, CleanupRetentionRule{}); }
    std::vector<CleanupTarget> cleanupPlan(const CleanupRetentionRule& rule) const;
    std::vector<CleanupTarget> cleanupPlan() const { return cleanupPlan(CleanupRetentionRule{}); }
    void autoAlignPartialClip();
    void previewBulkCleanup();
    bool exportCleanupPlan(const QString& path,const std::vector<CleanupTarget>& plan);
    void executeBulkCleanup(std::vector<CleanupTarget> plan);
    bool beginCleanupRecovery();
    bool markCleanupInProgress();
    void finishCleanupRecovery();
    void setReviewDecision(ReviewDecision decision);
    ReviewDecision pairDecision(int a,int b) const;
    ReviewDecision currentReviewDecision() const;
    void showGroupPosition(const QString& state={});
    void refreshReviewDecisions();
    bool protectedByReview(const VideoFileInfo& file) const;
    void updateReviewState();
    void deleteFile(PlayerPanel* panel);
    void addTransferActions(QMenu* menu,int index);
    void chooseTransfer(int index,const QString& operation);
    void chooseBatchTransfer(bool move,const QString& folder={});
    QFutureWatcherBase* batchTransferWatcher_=nullptr;
    void undoLastTransfer();
    void startTransfer(const VideoFileInfo& file,const QString& destination,const QString& operation,const TransferRecord* undo=nullptr);
    void refreshTransferResults();
    void updateUndoAction();
    void updateMediaMode();
    void toggleProtection(PlayerPanel* panel);
    void showFileDetails(PlayerPanel* panel);
    void playBoth(PlayerPanel* source=nullptr);
    void controlSeek(PlayerPanel* source,int64_t position,bool exact);
    void seekBoth(int64_t position, bool exact = true);
    std::pair<int64_t,int64_t> comparisonRange() const;
    void tick();
    void showScanReport();
    QString scanReportText() const;
    bool saveScanReportFile(const QString& path,const QString& body) const;
    void previewCleanupMarks();
    std::vector<CleanupTarget> markedCleanupPlan() const;
    void removeMarkedFiles();
    void ignoreCurrentPair();
    void showOperationHistory();
    void exportResults();
    bool exportResultsFile(const QString& path);
    void openTwoFiles();
    void applyFilter();
    int adjacentVisibleGroup(int fromGroup,int step) const;
    void updateGroupNavigationActions();
    enum class SessionSaveStatus { Saved, BusyOrInvalid, LowMemory, LimitExceeded, WriteFailed };
    void saveSession();
    void loadSession();
    bool saveSessionFile(const QString& path,SessionSaveStatus* status=nullptr);
    std::function<bool()> sessionSaveJob(const QString& path,std::shared_ptr<std::atomic<bool>> cancelled={},std::shared_ptr<SessionSaveStatus> status={});
    bool saveLastReview(bool force=false);
    bool loadSessionFile(const QString& path,bool rebuildLegacy=false);
    bool legacySessionNeedsScan_=false;
    void saveSettings();
    ScanCriteria activeCriteria() const;
    QAction* action(const char* key, const QKeySequence& shortcut, std::function<void()> callback);
    QSettings settings_;
    std::vector<std::wstring> folders_;
    ScanCriteria options_, scanCriteria_;
    ScanMetrics scanMetrics_;
    std::vector<VideoFileInfo> files_;
    std::vector<VideoGroup> groups_;
    std::vector<ScanIssue> scanIssues_;
    std::vector<std::pair<QAction*, const char*>> actionTexts_;
    std::thread scanThread_;
    std::atomic<bool> cancel_{false};
    bool scanning_ = false, pendingClose_ = false, syncing_ = false, manual_ = false;
    bool closing_=false,closeReady_=false;
    bool pausedScan_=false;
    bool scanPausable_=false;
    bool clearingCache_=false;
    bool fileOperationPending_=false;
    std::atomic<bool> bulkCancel_{false};
    bool sortingMetadata_=false;
    QFutureWatcher<PartialClipAlignment>* alignmentWatcher_=nullptr;
    QAction *pauseAction_,*resumeAction_;
    QAction *privacyAction_,*hideVideosAction_,*hideThumbnailsAction_;
    QAction* linkAction_;
    bool lastControlledRight_=false;
    std::optional<bool> linkedStepFromRight_;
    int currentGroup_ = -1;
    int64_t syncPosition_ = 0;
    int64_t alignmentOffset100ns_=0;
    QDoubleSpinBox* offset_;
    uint64_t generation_ = 0;
    QSet<int> pendingPreviews_;
    QHash<int,bool> previewRuns_; // finished preview attempts this scan; value = the attempt also sampled the video
    QSet<int> requestedSamples_;
    QSet<int> markedFiles_;
    QThreadPool previewPool_;
    QString cachePath_, cacheError_, cleanupReviewPath_;
    QAction* recoverCleanupAction_;
    QAction* undoTransferAction_;
    QAction *resumeReviewAction_,*rememberReviewAction_,*forgetReviewAction_=nullptr;
    QString lastReviewPath_;
    QTimer reviewSaveTimer_;
    bool reviewDirty_=false,reviewSaving_=false;
    bool reviewCloseAttempted_=false;
    QByteArray lastReviewToken_;
    std::shared_ptr<std::atomic<bool>> reviewSaveCancel_;
    QFutureWatcher<bool>* reviewSaveWatcher_=nullptr;
    QFutureWatcher<QString>* clearCacheWatcher_=nullptr;
    bool simulateLowMemorySave_=false;
    ReviewDecisions reviewDecisions_;
    std::set<std::string> deferredReviewFiles_;
    std::array<QAction*,4> reviewActions_{};
    unsigned cacheHits_=0;
    std::optional<std::vector<VideoFileInfo>> restoredFiles_;
    std::optional<std::vector<VideoGroup>> restoredGroups_;
    std::vector<ScanIssue> restoredIssues_;
    ScanMetrics restoredMetrics_;
    bool savedResultEvidence_=false;
    bool restoreManual_=false;
    QString restoreLeft_,restoreRight_;
    QHash<QString,QString> restoreRightCandidates_;
    int64_t restoreLeftTime_=0,restoreRightTime_=0;
    double restoreZoom_=1;
    double restoreOffset_=0;
    QPointF restorePan_;
    bool restoreLinked_=true;
    std::set<std::string> restoreMarks_;
    std::optional<std::pair<int64_t,int64_t>> restorePositions_;
    QTimer timer_;
    QElapsedTimer syncTimer_;
    QElapsedTimer scanElapsed_;
    qint64 lastScanElapsedMs_=0;
    QString lastScanOutcome_;
    bool scanCompletedThisSession_=false;
    QLabel* scanTimeLabel_;
    PlayerPanel *left_, *right_;
    QLabel *folderSummary_, *resultSummary_, *spaceSummary_, *groupLabel_, *similarity_;
    QComboBox *profile_, *sort_, *mediaMode_;
    QComboBox* referenceRule_=nullptr;
    void fillReferenceRules();
    void applyReferenceRule();
    void makeReference(int groupIndex,int slot);
    bool canBeReference(const VideoGroup& group,int member) const;
    bool matchesWholeGroup(const VideoGroup& group,int member) const;
    void readMetadata(std::vector<std::pair<int,VideoFileInfo>>& missing,std::atomic<bool>& cancel,std::atomic<int>* done=nullptr,bool sampleVideos=false);
    void mergeMetadata(std::vector<std::pair<int,VideoFileInfo>>& result);
    bool leftOnlySorting_=false;
    QSpinBox* similarityThreshold_=nullptr;
    QTreeWidget* results_=nullptr;
    int hoveredGroup_=-1,hoveredSlot_=-1;
    QLineEdit* filter_;
    QProgressBar* progress_;
    QSplitter* vertical_;
    QAction *foldersAction_, *rescanAction_, *stopAction_, *playBothAction_, *previousAction_, *nextAction_, *reportAction_;
};
