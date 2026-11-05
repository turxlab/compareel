#pragma once
#include "platform/PlatformMedia.h"
// Shared by MainWindow.cpp, PlayerPanel.cpp and MainWindowChecks.cpp only.
#include "MainWindow.h"
#include "Language.h"
#include "Icons.h"
#include "FileOperations.h"
#include "PathKey.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QKeySequence>
#include <QStandardPaths>
#include <QToolButton>
#include <QTreeWidget>
#include <array>
#include <functional>

namespace MainWindowDetail {
using Language::text;
inline constexpr std::array<const char*,4> reviewLabels{"Unreviewed","Reviewed","Keep both files","Review later"};inline QString readError(HRESULT error) {
    auto code="0x"+QString::number(quint32(error),16).rightJustified(8,'0');
    if(error==Platform::kErrorCodecNotFound)
        return Language::text("Neither the system decoder nor the local FFmpeg reader could decode this file. It was skipped.")+" ("+code+")";
    if(error==Platform::kErrorUnsupportedByteStream || error==Platform::kErrorInvalidMediaType)
        return Language::text("Unsupported media container or stream format. This file cannot be decoded locally and was skipped.")+" ("+code+")";
    if(error==HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE))
        return Language::text("File exceeds size or frame/page safety limits and was skipped.")+" ("+code+")";
    if(error==HRESULT_FROM_WIN32(ERROR_INVALID_DATA))
        return Language::text("Corrupt or unreadable media data. This file was skipped.")+" ("+code+")";
    if(error==HRESULT_FROM_WIN32(ERROR_DEV_NOT_EXIST) || error==HRESULT_FROM_WIN32(ERROR_BAD_NETPATH) ||
       error==HRESULT_FROM_WIN32(ERROR_NETNAME_DELETED) || error==HRESULT_FROM_WIN32(ERROR_UNEXP_NET_ERR) ||
       error==HRESULT_FROM_WIN32(ERROR_SEM_TIMEOUT) || error==HRESULT_FROM_WIN32(ERROR_NOT_READY))
        return Language::text("Storage device or network share disconnected or timed out while reading this file.")+" ("+code+")";
    return QString::fromStdWString(FileOperations::Result{error}.message())+" ("+code+")";
}
inline QString settingsPath(const QString& directory) {
    QString path = directory.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) : directory;
    QDir().mkpath(path);
    return QDir(path).filePath("settings.ini");
}
inline QStringList strings(const std::vector<std::wstring>& paths) {
    QStringList list;
    for (const auto& path : paths) list << QString::fromStdWString(path);
    return list;
}
inline std::vector<std::wstring> paths(const QStringList& list) {
    std::vector<std::wstring> result;
    for (const auto& path : list) result.push_back(QDir::toNativeSeparators(path).toStdWString());
    return result;
}
inline QToolButton* tool(QWidget* parent, Glyph glyph, const char* label, std::function<void()> clicked) {
    auto button = new QToolButton(parent);
    button->setIcon(icon(glyph));
    button->setIconSize(QSize(18,18));
    button->setFixedSize(28,28);
    button->setToolTip(text(label));
    button->setAccessibleName(text(label));
    QObject::connect(button, &QToolButton::clicked, parent, std::move(clicked));
    return button;
}
// Shortcut hints in translated text are written as on Windows ("Ctrl+L"); macOS shows them as "⌘L".
// Mac keyboards have no forward Delete key; Finder moves files to the Trash with ⌘⌫, so that is added there.
inline QList<QKeySequence> deleteShortcuts() {
#ifdef Q_OS_MACOS
    return {QKeySequence(Qt::Key_Delete),QKeySequence(Qt::CTRL|Qt::Key_Backspace)};
#else
    return {QKeySequence(Qt::Key_Delete)};
#endif
}
inline QString nativeShortcuts(QString value) {
    for(auto shortcut:{"Ctrl+Shift+,","Ctrl+L"}) value.replace(shortcut,QKeySequence(shortcut).toString(QKeySequence::NativeText));
    return value;
}
// The project home: source, releases and the update check.
inline constexpr const char* projectUrl="https://github.com/turxlab/compareel";
inline constexpr const char* guideUrl="https://turxlab.github.io/compareel/";
// COMPAREEL_TRACE=1 prints how long the steps of slow operations take, to diagnose them on a user's machine.
inline void trace(const char* step,const QElapsedTimer& clock) {
    static const bool on=qEnvironmentVariableIsSet("COMPAREEL_TRACE");
    if(on) qInfo("trace %6lld ms  %s",static_cast<long long>(clock.elapsed()),step);
}
inline QString duration(int64_t value) { return QString::fromStdWString(FormatDuration(value)); }
// Removal rows of the grouped cleanup plan (children of each kept file).
inline int planRows(QTreeWidget* tree) { int rows=0; if(tree) for(int i=0;i<tree->topLevelItemCount();++i) rows+=tree->topLevelItem(i)->childCount(); return rows; }
// Sources can be folders or individual files.
inline QString sourceSummary(const std::vector<std::wstring>& sources) {
    size_t files=0; for(const auto& source:sources) if(QFileInfo(QString::fromStdWString(source)).isFile()) ++files;
    const size_t folders=sources.size()-files;
    QStringList parts;
    if(folders) parts<<(folders==1?text("1 folder"):text("%1 folders").arg(folders));
    if(files) parts<<(files==1?text("1 file"):text("%1 files").arg(files));
    return parts.isEmpty()?text("No sources selected"):parts.join(" · ");
}
// In the toolbar the source count doubles as a link that reopens the source list.
inline QString sourceLink(const std::vector<std::wstring>& sources) {
    return QString("<a href=\"sources\" style=\"color:%2; text-decoration:none\">%1</a>").arg(sourceSummary(sources).toHtmlEscaped(),Theme::color("@sourceLink").name());
}
inline QString scanDuration(qint64 milliseconds) {
    auto seconds=std::max<qint64>(0,milliseconds)/1000;
    return QString("%1:%2:%3").arg(seconds/3600,2,10,QChar('0')).arg(seconds/60%60,2,10,QChar('0')).arg(seconds%60,2,10,QChar('0'));
}
inline QString thumbnailTip(const VideoFileInfo& file) {
    QString extra;
    if(StillImages::supported(file.fullPath) && file.imageFrameCount>1) {
        if(file.imageKind==ImageEvidenceKind::AnimatedImage) extra=" · "+text("%1 frames").arg(file.imageFrameCount);
        else if(file.imageKind==ImageEvidenceKind::MultiPageDocument) extra=" · "+text("%1 pages").arg(file.imageFrameCount);
        else if(file.imageKind==ImageEvidenceKind::MultiSizeIcon) extra=" · "+text("%1 sizes").arg(file.imageFrameCount);
    }
    return QString::fromStdWString(file.fullPath)+"\n"+QString::fromStdWString(FormatBytesShort(file.fileSize))+" · "+
        (StillImages::supported(file.fullPath)?QString():duration(file.duration100ns)+" · ")+QString("%1 × %2").arg(file.width).arg(file.height)+extra+
        (file.protectedFile?" · "+text("Protected"):QString());
}
inline QString csvEvidenceTag(const VideoFileInfo& file, const ScanCriteria& criteria, bool manual) {
    if(manual) return "manual";
    if(criteria.matchContent) return "sha256";
    if(criteria.images && criteria.matchFirstFrame) {
        switch(file.imageKind) {
            case ImageEvidenceKind::AnimatedImage: return "visual_animation";
            case ImageEvidenceKind::MultiPageDocument: return "visual_multipage";
            case ImageEvidenceKind::MultiSizeIcon: return "visual_icon";
            case ImageEvidenceKind::CompositeDocument: return "visual_psd_composite";
            case ImageEvidenceKind::TonemappedHdr: return "visual_hdr_tonemap";
            case ImageEvidenceKind::RawEmbeddedPreview: return "visual_raw_preview";
            case ImageEvidenceKind::RawDeveloped: return "visual_raw_developed";
            case ImageEvidenceKind::VectorRasterized: return "visual_svg_raster";
            default: return "visual_image";
        }
    }
    if(criteria.matchVisualSamples) return "visual_samples";
    if((criteria.matchFileName || criteria.matchExtension) && !criteria.matchSize && !criteria.matchDuration && !criteria.matchResolution)
        return "filename_candidate";
    return "metadata_candidate";
}
}
using namespace MainWindowDetail;
