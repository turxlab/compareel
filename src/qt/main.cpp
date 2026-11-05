#include "MainWindow.h"
#include "ShellIntegration.h"
#include "Theme.h"
#include "Language.h"
#include "platform/Platform.h"
#include <QDir>
#include <QSettings>
#include <QApplication>
#include <algorithm>
#include <utility>
#include <vector>
#include <QCommandLineParser>
#include <QFileOpenEvent>
#include <QFont>
#include <QIcon>
#include <QImageReader>
#include <QPalette>
#include <QStyleFactory>
#include <QThreadPool>
#include <QTimer>
#include <QTranslator>
#include <QTemporaryDir>
#include <QTextDocument>

// Qt shows a plain-text tooltip on a single line, however long; as rich text it wraps to a readable width.
// The same filter tidies window titles.
class ToolTipWrapper final : public QObject {
public:
    using QObject::QObject;
    bool eventFilter(QObject* object,QEvent* event) override {
        // Dialogs are titled after the command that opens them, but a window title never ends in an ellipsis.
        if(event->type()==QEvent::WindowTitleChange) if(auto widget=qobject_cast<QWidget*>(object); widget && widget->isWindow() && widget->windowTitle().endsWith(QChar(0x2026)))
            widget->setWindowTitle(widget->windowTitle().chopped(1).trimmed());
        if(event->type()==QEvent::ToolTipChange) if(auto widget=qobject_cast<QWidget*>(object)) {
            const auto tip=widget->toolTip();
            if(tip.size()>100 && !Qt::mightBeRichText(tip)) widget->setToolTip("<p>"+tip.toHtmlEscaped().replace('\n',"<br>")+"</p>");
        }
        return false;
    }
};

// macOS hands files opened from Finder or dropped on the Dock icon to the running app as events, one per file;
// files from one gesture arrive together, so they are collected and opened as one set.
class FileOpenHandler final : public QObject {
public:
    using QObject::QObject;
    MainWindow* window=nullptr;
    bool eventFilter(QObject* object,QEvent* event) override {
        if(event->type()!=QEvent::FileOpen) return QObject::eventFilter(object,event);
        const auto path=static_cast<QFileOpenEvent*>(event)->file(); if(path.isEmpty()) return false;
        if(pending_.isEmpty()) QTimer::singleShot(0,this,[this] { deliver(); });
        pending_<<path; return true;
    }
private:
    QStringList pending_;
    void deliver() {
        if(!window) { QTimer::singleShot(50,this,[this] { deliver(); }); return; }
        const auto paths=std::exchange(pending_,{});
        window->showNormal(); window->raise(); window->activateWindow(); window->openPaths(paths);
    }
};

#ifdef Q_OS_MACOS
void DisableAutomaticFullScreenMenuItem(); // MacMenu.mm

// Qt builds the macOS application menu (About, Settings, Services, Hide, Quit) from its own English strings
// and asks the translators for them each time the menu bar is updated; they come from the language files.
class ApplicationMenuTranslator final : public QTranslator {
public:
    using QTranslator::QTranslator;
    bool isEmpty() const override { return false; }
    QString translate(const char* context,const char* source,const char*,int) const override {
        if(qstrcmp(context,"MAC_APPLICATION_MENU")!=0) return {};
        if(qstrcmp(source,"About %1")==0) return Language::text("About Compareel").replace("Compareel","%1");
        if(qstrcmp(source,"Preferences...")==0) return Language::text("Settings…");
        for(const char* key:{"Services","Hide %1","Hide Others","Show All","Quit %1"}) if(qstrcmp(source,key)==0) return Language::text(key);
        return {};
    }
};
#endif

int main(int argc,char** argv) {
    Platform::ComInit com(Platform::ComApartment::ApartmentThreaded);
    if(!com.succeeded()) return 2;
    Platform::MediaRuntime media;
    if(!media.succeeded()) return 3;
    int result=0;
    {
#ifdef Q_OS_MACOS
        DisableAutomaticFullScreenMenuItem();
#endif
        QApplication app(argc,argv);
        QImageReader::setAllocationLimit(256);
        ToolTipWrapper toolTipWrapper; app.installEventFilter(&toolTipWrapper);
        FileOpenHandler fileOpenHandler; app.installEventFilter(&fileOpenHandler);
#ifdef Q_OS_MACOS
        ApplicationMenuTranslator applicationMenuTranslator; app.installTranslator(&applicationMenuTranslator);
#endif
        // On macOS the bundle's icon (used by About and the Dock) has the system's rounded-square shape; the .ico has not.
#ifdef Q_OS_MACOS
        app.setWindowIcon(QIcon(QCoreApplication::applicationDirPath()+"/../Resources/app.icns"));
#else
        app.setWindowIcon(QIcon(":/branding/app.ico"));
#endif
        // No organization name: the data folder is then %LOCALAPPDATA%\Compareel (macOS: Application Support/Compareel,
        // Linux: ~/.local/share/Compareel) instead of Compareel\Compareel.
        app.setApplicationName("Compareel"); app.setApplicationVersion(COMPAREEL_DISPLAY_VERSION);
        Theme::apply(app,"dark");
        QCommandLineParser parser;
        parser.addHelpOption(); parser.addVersionOption();
        parser.addPositionalArgument("paths","Two images or two videos to compare side by side, or folders and files to scan.","[paths...]");
        // Self-checks and crash children are for development; they stay out of --help.
        auto internal=[&parser](QCommandLineOption option) { option.setFlags(QCommandLineOption::HiddenFromHelp); parser.addOption(option); };
        internal({"open-pair-check","Check command-line two-file opening with private generated media."});
        internal({"cache-policy-check","Check disk media cache opt-out through the actual UI with generated images."});
        internal({"evidence-check","Check SHA evidence and filters through actual UI with generated images."});
        internal({"heif-ui-check","Check HEIC/AVIF scanning and sessions with private copies of two public fixtures."});
        internal({"audio-evidence-ui-check","Check the sampled-audio menu with two generated fixture videos."});
        internal({"partial-search-ui-check","Check folder-wide partial search using a generated fixture folder."});
        internal({"image-guard-check","Check generated animation playback, pages and independent left-file order."});
        internal({"batch-transfer-check","Check batch copy/move and conflict naming with generated files."});
        internal({"branding-check","Check Compareel icons, About and session format checks without media."});
        internal({"session-scale-check","Check 100000-file session save/load and limit rejection without media."});
        internal({"appearance-check","Photograph every theme on the welcome card, Settings (EN/TR) and a loaded comparison."});
        internal({"latency-check","Measure review navigation, decision, mark and ignore latency on a large private result list."});
        internal({"session-snapshot-check","Check direct reopening of 557 groups/1299 results in 40003 inputs with an empty cache."});
        internal({"continuation-check","Check frozen scan restart/resume using private fixture copies, without screenshots."});
        internal({"transport-check","Check larger time labels and expanded slider hit areas through actual mouse events, without screenshots."});
        internal({"criteria-check","Check custom profile activation, settings Save and Cancel without media."});
        internal({"path-check","Check Unicode paths over 300 characters with private fixture copies; no screenshots."});
        internal({"cleanup-recovery-check","Check interrupted real UI batch and review recovery with private copies; no screenshots."});
        internal({"cleanup-crash-child","Internal self-check: abruptly exit during a private batch."});
        internal({"review-decisions-check","Check persistent review decisions and removal guards with private copies; no screenshots."});
        internal({"transfer-check","Check actual transfer menus, copy/move/quarantine and persistent Undo with private copies; no screenshots."});
        internal({"image-check","Check real image scanning, comparison, sessions, privacy and file operations with generated images; no screenshots."});
        internal({"review-resume-check","Check abrupt-exit review recovery and safe autosave with private copies; no screenshots."});
        internal({"review-resume-child","Internal self-check: persist a private review, then exit abruptly."});
        internal({"smoke-check","Initialize Qt, COM and Media Foundation, then exit."});
        internal({"crash-scan-child","Internal self-check: abruptly exit after launching an isolated scan."});
        internal({"freeze-list","Internal crash check: publish a frozen list before exiting."});
        internal({"playback-check","Check 60 seconds of linked CFR/VFR playback with silent audio fixtures."});
        internal({"playback-seconds","Duration of the linked playback observation (60..86400).","seconds","60"});
        internal({"subtitle-check","Check asynchronous embedded subtitles in the real details dialog with bundled FFmpeg."});
        internal({"ui-check","Run the real scan/play/seek/recycle workflow with private media copies."});
        internal({"results-check","Check asynchronous sorting and visible thumbnails with private media copies."});
        internal({"matching-check","Check cross-folder matching, transforms, cache/session and metadata through the real UI."});
        internal({"feedback-check","Check privacy, keyboard navigation, captions, similarity and compact layout."});
        internal({"controls-cleanup-check","Check linked controls, Lucide and safe all-group recycling with private copies."});
        internal({"timing-check","Check persistent elapsed scan timing with private media copies."});
        internal({"review-tools-check","Check accessibility/scaling, partial-clip alignment and bulk retention rules with private media copies."});
        internal({"media","Media fixture for ui-check.","path"});
        internal({"screenshot","PNG output for ui-check.","path"});
        internal({"data-dir","Override settings/cache directory.","path"});
        parser.process(app);
        auto dataDirectory=parser.value("data-dir"); std::optional<QTemporaryDir> checkState;
        if((parser.isSet("cache-policy-check") || parser.isSet("evidence-check") || parser.isSet("heif-ui-check") || parser.isSet("audio-evidence-ui-check") || parser.isSet("partial-search-ui-check") || parser.isSet("image-guard-check") || parser.isSet("batch-transfer-check")) && dataDirectory.isEmpty()) { checkState.emplace(); dataDirectory=checkState->path(); }
        const QStringList checks{"branding-check","smoke-check","ui-check","results-check","matching-check","review-tools-check","feedback-check","controls-cleanup-check","timing-check","crash-scan-child","playback-check","subtitle-check","session-scale-check","continuation-check","transport-check","latency-check","appearance-check"};
        if(dataDirectory.isEmpty() && (parser.isSet("session-snapshot-check") || parser.isSet("open-pair-check") || parser.isSet("review-resume-check") || parser.isSet("image-check") || parser.isSet("transfer-check") || parser.isSet("review-decisions-check") || parser.isSet("cleanup-recovery-check") || parser.isSet("path-check") || parser.isSet("criteria-check") || std::any_of(checks.begin(),checks.end(),[&](const auto& option) { return parser.isSet(option); }))) {
            checkState.emplace(); dataDirectory=checkState->path();
        }
        // Checks run on fresh profiles (and open more windows on others) and read English text, so a profile without
        // a language starts in English there instead of the system language; checks of other languages switch themselves.
        if(checkState || !parser.value("data-dir").isEmpty()) Language::defaultCode()="en";
        if(checkState && !checkState->isValid()) result=4;
        else
        {
            MainWindow window(dataDirectory);
            if(parser.isSet("review-resume-child")) {
                if(parser.value("data-dir").isEmpty() || parser.value("media").isEmpty()) result=4;
                else window.runReviewResumeChild(parser.value("media"));
            } else if(parser.isSet("review-resume-check")) {
                window.show(); QApplication::processEvents(); result=window.runReviewResumeCheck(parser.value("media"))?0:4;
            } else if(parser.isSet("image-check")) {
                window.show(); QApplication::processEvents(); result=window.runImageCheck()?0:4;
            } else if(parser.isSet("transfer-check")) {
                window.show(); QApplication::processEvents(); result=window.runTransferCheck(parser.value("media"))?0:4;
            } else if(parser.isSet("review-decisions-check")) {
                window.show(); QApplication::processEvents(); result=window.runReviewDecisionsCheck(parser.value("media"))?0:4;
            } else if(parser.isSet("cleanup-crash-child")) {
                if(parser.value("data-dir").isEmpty() || parser.value("media").isEmpty()) result=4;
                else window.runCleanupCrashChild(parser.value("media"));
            } else if(parser.isSet("cleanup-recovery-check")) {
                result=window.runCleanupRecoveryCheck(parser.value("media"))?0:4;
            } else if(parser.isSet("path-check")) {
                window.show(); QApplication::processEvents(); result=window.runPathCheck(parser.value("media"))?0:4;
            } else if(parser.isSet("criteria-check")) {
                window.show(); QApplication::processEvents(); result=window.runCriteriaCheck()?0:4;
            } else if(parser.isSet("transport-check")) {
                window.show(); QApplication::processEvents(); result=window.runTransportCheck(parser.value("media"))?0:4;
            } else if(parser.isSet("branding-check")) {
                window.show(); QApplication::processEvents();
                result=window.runBrandingCheck(parser.value("screenshot"))?0:4;
            } else if(parser.isSet("continuation-check")) {
                result=window.runContinuationCheck(parser.value("media"))?0:4;
            } else if(parser.isSet("session-scale-check")) {
                result=window.runSessionScaleCheck()?0:4;
            } else if(parser.isSet("latency-check")) {
                window.show(); QApplication::processEvents(); result=window.runLatencyCheck(parser.value("media"))?0:4;
            } else if(parser.isSet("session-snapshot-check")) {
                window.show(); result=window.runSessionSnapshotCheck()?0:4;
            } else if(parser.isSet("crash-scan-child")) {
                if(parser.value("data-dir").isEmpty() || parser.value("media").isEmpty()) result=4;
                else window.runCrashScanChild(parser.value("media"),parser.isSet("freeze-list"));
            } else if(parser.isSet("subtitle-check")) {
                window.show(); QApplication::processEvents();
                result=window.runSubtitleUiCheck(parser.value("media"),parser.value("screenshot"))?0:4;
            } else if(parser.isSet("playback-check")) {
                window.show(); QApplication::processEvents();
                bool secondsOk=false; const int seconds=parser.value("playback-seconds").toInt(&secondsOk);
                result=secondsOk && seconds>=60 && seconds<=86400 && window.runPlaybackCheck(parser.value("media"),parser.value("screenshot"),seconds)?0:4;
            } else if(parser.isSet("review-tools-check")) {
                window.show(); QApplication::processEvents();
                result=window.runReviewToolsCheck(parser.value("media"),parser.value("screenshot"))?0:4;
            } else if(parser.isSet("matching-check")) {
                window.show(); QApplication::processEvents();
                result=window.runMatchingCheck(parser.value("media"),parser.value("screenshot"))?0:4;
            } else if(parser.isSet("controls-cleanup-check")) {
                window.show(); QApplication::processEvents();
                result=window.runControlsCleanupCheck(parser.value("media"),parser.value("screenshot"))?0:4;
            } else if(parser.isSet("timing-check")) {
                window.show(); QApplication::processEvents();
                result=window.runTimingCheck(parser.value("media"),parser.value("screenshot"))?0:4;
            } else if(parser.isSet("feedback-check")) {
                window.show(); QApplication::processEvents();
                result=window.runFeedbackCheck(parser.value("media"),parser.value("screenshot"))?0:4;
            } else if(parser.isSet("appearance-check")) {
                window.show(); QApplication::processEvents();
                result=window.runAppearanceCheck(parser.value("media"),parser.value("screenshot"))?0:4;
            } else if(parser.isSet("results-check")) {
                window.show(); QApplication::processEvents();
                result=window.runResultsCheck(parser.value("media"),parser.value("screenshot"))?0:4;
            } else if(parser.isSet("ui-check")) {
                window.show(); QApplication::processEvents();
                result=window.runUiCheck(parser.value("media"),parser.value("screenshot"))?0:4;
            } else if(parser.isSet("open-pair-check")) {
                window.show(); result=window.runOpenPairCheck(parser.positionalArguments())?0:4;
            } else if(parser.isSet("cache-policy-check")) {
                window.show(); result=window.runCachePolicyCheck()?0:4;
            } else if(parser.isSet("evidence-check")) {
                window.show(); result=window.runEvidenceCheck()?0:4;
            } else if(parser.isSet("heif-ui-check")) {
                window.show(); result=window.runHeifUiCheck(parser.positionalArguments())?0:4;
            } else if(parser.isSet("audio-evidence-ui-check")) {
                window.show(); result=window.runAudioEvidenceUiCheck(parser.positionalArguments())?0:4;
            } else if(parser.isSet("partial-search-ui-check")) {
                window.show(); result=window.runPartialSearchUiCheck(parser.positionalArguments())?0:4;
            } else if(parser.isSet("image-guard-check")) {
                window.show(); result=window.runImageGuardCheck(parser.positionalArguments())?0:4;
            } else if(parser.isSet("batch-transfer-check")) {
                window.show(); result=window.runBatchTransferCheck()?0:4;
            } else if(!parser.isSet("smoke-check")) {
                ShellIntegration::refresh(); window.show(); fileOpenHandler.window=&window;
                // Checks and their harnesses run with a data folder of their own and never go online.
                if(!checkState && parser.value("data-dir").isEmpty()) window.startUpdateChecks();
                if(!parser.positionalArguments().isEmpty()) window.openPaths(parser.positionalArguments());
                result=app.exec(); fileOpenHandler.window=nullptr;
            }
        }
        QThreadPool::globalInstance()->waitForDone();
    }
    return result;
}
