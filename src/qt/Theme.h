#pragma once
#include <QApplication>
#include <QComboBox>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QHash>
#include <QPalette>
#include <QProxyStyle>
#include <QStyleFactory>
#include <QStyleHints>
#include <QStyledItemDelegate>
#include <QTemporaryDir>
#include <algorithm>
#include <utility>
#include <vector>

// Design tokens: every theme colour and type size is defined once per theme and substituted into the
// stylesheet. Widgets that paint themselves read the same tokens through Theme::color(), so switching
// themes only needs the stylesheet re-applied and the affected widgets repainted.
namespace Theme {
using Tokens=std::vector<std::pair<const char*,const char*>>;
struct Info { const char* id; const char* name; Tokens tokens; };

inline const std::vector<Info>& themes() {
    static const std::vector<Info> all{
        {"dark","Dark",{
            {"@surface","#111720"},{"@raised","#18212e"},{"@sunken","#141c27"},{"@field","#182230"},{"@base","#141d29"},
            {"@control","#243348"},{"@controlHover","#30445d"},{"@hover","#2a394c"},{"@hoverBorder","#42556e"},{"@pressed","#344962"},{"@focusBg","#263d50"},
            {"@border","#2a3749"},{"@borderStrong","#3b4c62"},{"@fieldBorder","#344359"},{"@fieldFocus","#63a0bd"},{"@rowLine","#202d3d"},{"@indicatorBorder","#5b6f88"},
            {"@text","#dae2ed"},{"@textStrong","#eef3fa"},{"@muted","#94a3b8"},{"@faint","#596679"},{"@soft","#a9b8cb"},{"@placeholder","#8191a7"},{"@statusText","#8596ac"},{"@headerText","#9eafc4"},
            {"@accent","#2d6973"},{"@accentHover","#387d88"},{"@accentBorder","#47828b"},{"@accentSubtle","#234e58"},{"@accentLine","#619ba5"},{"@accentText","#d8f4ef"},{"@onAccent","#eef3fa"},
            {"@focus","#80c9e8"},{"@link","#80c9e8"},{"@selection","#365c79"},{"@highlight","#2d5c76"},{"@highlightText","#ffffff"},{"@rowSelected","#233b4d"},
            {"@menu","#1d2938"},{"@menuSelected","#2c4259"},{"@header","#1d2938"},{"@statusLine","#263346"},{"@tooltip","#253346"},{"@tooltipBorder","#4e6681"},{"@tooltipText","#ffffff"},
            {"@spin","#2a3b50"},{"@spinBorder","#52647b"},{"@spinHover","#41617b"},{"@spinPressed","#527a94"},
            {"@groove","#344359"},{"@sliderFill","#619cad"},{"@handle","#b3d4de"},{"@progress","#68b7ab"},
            {"@roleRef","#234e58"},{"@roleRefText","#bfe8e2"},{"@roleCand","#1f3f52"},{"@roleCandText","#bfe3f3"},
            {"@success","#c9f0dd"},{"@successBg","#1d4535"},{"@successBorder","#3f8f6c"},
            {"@warning","#f3dfae"},{"@warningBg","#463818"},{"@warningBorder","#9a7a35"},
            {"@danger","#ffd9dc"},{"@dangerBg","#6e2630"},{"@dangerHover","#86303b"},{"@dangerBorder","#a8434e"},
            {"@icon","#ccd5e2"},{"@iconAccent","#8fb8c8"},{"@iconMuted","#7f8b9e"},{"@iconActive","#78c5ae"},{"@keepIcon","#8fd3a8"},{"@removeIcon","#c98a90"},{"@proofText","#9fd9bd"},
            {"@sourceLink","#9fc3d6"},{"@thumbHover","#8bbbc9"},{"@thumbFocus","#e1cf91"},{"@media","#090d13"},{"@mediaFrame","#0b0e13"},{"@mediaText","#647187"},{"@thumbBorder","#283748"},{"@grip","#5b6f88"},{"@chevron","#e0e9f3"},{"@thumbSelected","#70c5e0"},{"@thumbMarked","#c96a6a"},{"@thumbReference","#4c837f"},{"@checkMark","#eef3fa"}}},
        {"graphite","Graphite",{
            {"@surface","#1b1b1d"},{"@raised","#232326"},{"@sunken","#1f1f22"},{"@field","#252528"},{"@base","#1e1e21"},
            {"@control","#313136"},{"@controlHover","#3c3c42"},{"@hover","#333338"},{"@hoverBorder","#4a4a52"},{"@pressed","#43434a"},{"@focusBg","#2e3640"},
            {"@border","#333338"},{"@borderStrong","#48484f"},{"@fieldBorder","#45454c"},{"@fieldFocus","#6d9fd6"},{"@rowLine","#2b2b2f"},{"@indicatorBorder","#6a6a73"},
            {"@text","#e3e3e6"},{"@textStrong","#f4f4f6"},{"@muted","#a0a0a8"},{"@faint","#63636b"},{"@soft","#b4b4bb"},{"@placeholder","#8a8a92"},{"@statusText","#94949c"},{"@headerText","#a8a8b0"},
            {"@accent","#2f6db5"},{"@accentHover","#3a7cc6"},{"@accentBorder","#4f8bd0"},{"@accentSubtle","#263c57"},{"@accentLine","#5e93d1"},{"@accentText","#dce9f8"},{"@onAccent","#ffffff"},
            {"@focus","#8ab8ee"},{"@link","#8ab8ee"},{"@selection","#2f4f78"},{"@highlight","#2f5a8f"},{"@highlightText","#ffffff"},{"@rowSelected","#2a3442"},
            {"@menu","#262629"},{"@menuSelected","#34404f"},{"@header","#262629"},{"@statusLine","#2e2e33"},{"@tooltip","#2c2c30"},{"@tooltipBorder","#55555d"},{"@tooltipText","#f4f4f6"},
            {"@spin","#34343a"},{"@spinBorder","#55555d"},{"@spinHover","#44505f"},{"@spinPressed","#50637a"},
            {"@groove","#45454c"},{"@sliderFill","#5e93d1"},{"@handle","#c9daf0"},{"@progress","#5e93d1"},
            {"@roleRef","#263c57"},{"@roleRefText","#cfe0f5"},{"@roleCand","#2e3540"},{"@roleCandText","#d3dbe6"},
            {"@success","#c9f0dd"},{"@successBg","#1f3d30"},{"@successBorder","#3f8f6c"},
            {"@warning","#f3dfae"},{"@warningBg","#3f3418"},{"@warningBorder","#9a7a35"},
            {"@danger","#ffd9dc"},{"@dangerBg","#5e2329"},{"@dangerHover","#77303a"},{"@dangerBorder","#a8434e"},
            {"@icon","#d0d0d6"},{"@iconAccent","#8ab8ee"},{"@iconMuted","#85858d"},{"@iconActive","#7fc4a8"},{"@keepIcon","#8fd3a8"},{"@removeIcon","#d08a90"},{"@proofText","#9fd9bd"},
            {"@sourceLink","#a9c6ea"},{"@thumbHover","#8ab8ee"},{"@thumbFocus","#e1cf91"},{"@media","#0c0c0d"},{"@mediaFrame","#0e0e10"},{"@mediaText","#6b6b73"},{"@thumbBorder","#38383e"},{"@grip","#6a6a73"},{"@chevron","#e3e3e6"},{"@thumbSelected","#70c5e0"},{"@thumbMarked","#c96a6a"},{"@thumbReference","#4c837f"},{"@checkMark","#ffffff"}}},
        {"light","Light",{
            {"@surface","#f3f5f8"},{"@raised","#ffffff"},{"@sunken","#eef1f5"},{"@field","#ffffff"},{"@base","#ffffff"},
            {"@control","#e9edf2"},{"@controlHover","#dde3ea"},{"@hover","#e4e9ef"},{"@hoverBorder","#c3ccd7"},{"@pressed","#d3dbe4"},{"@focusBg","#e1eef3"},
            {"@border","#d8dee6"},{"@borderStrong","#c0c9d4"},{"@fieldBorder","#c3ccd7"},{"@fieldFocus","#2f7f8c"},{"@rowLine","#e8ecf1"},{"@indicatorBorder","#8d9aab"},
            {"@text","#1f2933"},{"@textStrong","#0f1720"},{"@muted","#5b6878"},{"@faint","#9aa5b3"},{"@soft","#4a5664"},{"@placeholder","#8a96a5"},{"@statusText","#5b6878"},{"@headerText","#4a5664"},
            {"@accent","#1f7480"},{"@accentHover","#1a6570"},{"@accentBorder","#1a6570"},{"@accentSubtle","#d6ecee"},{"@accentLine","#4f9aa4"},{"@accentText","#0f4b53"},{"@onAccent","#ffffff"},
            {"@focus","#2b8ca8"},{"@link","#1d6f8a"},{"@selection","#bfdde3"},{"@highlight","#bfdde3"},{"@highlightText","#0f1720"},{"@rowSelected","#dbeef1"},
            {"@menu","#ffffff"},{"@menuSelected","#e1eef1"},{"@header","#eef1f5"},{"@statusLine","#d8dee6"},{"@tooltip","#ffffff"},{"@tooltipBorder","#b8c2ce"},{"@tooltipText","#1f2933"},
            {"@spin","#e9edf2"},{"@spinBorder","#c3ccd7"},{"@spinHover","#d6e3ea"},{"@spinPressed","#c4d6df"},
            {"@groove","#cfd6df"},{"@sliderFill","#2f8f9c"},{"@handle","#1f7480"},{"@progress","#2f9c8f"},
            {"@roleRef","#d6ecee"},{"@roleRefText","#0f4b53"},{"@roleCand","#dbe9f3"},{"@roleCandText","#174766"},
            {"@success","#1f6b46"},{"@successBg","#dff3e8"},{"@successBorder","#8cc7a8"},
            {"@warning","#7a5a10"},{"@warningBg","#fbf0d4"},{"@warningBorder","#e0c27a"},
            {"@danger","#9b1c28"},{"@dangerBg","#fbe1e3"},{"@dangerHover","#f6cfd3"},{"@dangerBorder","#e39aa2"},
            {"@icon","#3c4855"},{"@iconAccent","#2f7f8c"},{"@iconMuted","#8592a2"},{"@iconActive","#1f8a6a"},{"@keepIcon","#2f9a5e"},{"@removeIcon","#c0505b"},{"@proofText","#1f7a4c"},
            {"@sourceLink","#1d6f8a"},{"@thumbHover","#4f9aa4"},{"@thumbFocus","#b8860b"},{"@media","#14181d"},{"@mediaFrame","#1a1f26"},{"@mediaText","#7a8696"},{"@thumbBorder","#c9d1db"},{"@grip","#9aa5b3"},{"@chevron","#3c4855"},{"@thumbSelected","#1f7a8c"},{"@thumbMarked","#c0505b"},{"@thumbReference","#2f7a6a"},{"@checkMark","#ffffff"}}},
        {"sand","Sand",{
            {"@surface","#f4efe6"},{"@raised","#fbf8f2"},{"@sunken","#efe8dc"},{"@field","#fffdf9"},{"@base","#fffdf9"},
            {"@control","#ebe3d5"},{"@controlHover","#e2d8c6"},{"@hover","#e8dfcf"},{"@hoverBorder","#cbbda5"},{"@pressed","#dacdb6"},{"@focusBg","#efe3cf"},
            {"@border","#e0d6c5"},{"@borderStrong","#cdbfa8"},{"@fieldBorder","#d0c3ad"},{"@fieldFocus","#9a6a2f"},{"@rowLine","#ece4d6"},{"@indicatorBorder","#a39581"},
            {"@text","#2e2721"},{"@textStrong","#1e1914"},{"@muted","#6f6355"},{"@faint","#a89a87"},{"@soft","#594e42"},{"@placeholder","#9a8c79"},{"@statusText","#6f6355"},{"@headerText","#594e42"},
            {"@accent","#9a5b23"},{"@accentHover","#87501e"},{"@accentBorder","#87501e"},{"@accentSubtle","#f1dfc8"},{"@accentLine","#c08a52"},{"@accentText","#5c3612"},{"@onAccent","#ffffff"},
            {"@focus","#b5762f"},{"@link","#a0581c"},{"@selection","#ecd4b5"},{"@highlight","#ecd4b5"},{"@highlightText","#1e1914"},{"@rowSelected","#f0e0c9"},
            {"@menu","#fbf8f2"},{"@menuSelected","#efe1cc"},{"@header","#efe8dc"},{"@statusLine","#e0d6c5"},{"@tooltip","#fbf8f2"},{"@tooltipBorder","#c9b99f"},{"@tooltipText","#2e2721"},
            {"@spin","#ebe3d5"},{"@spinBorder","#d0c3ad"},{"@spinHover","#e6d5bb"},{"@spinPressed","#dcc6a5"},
            {"@groove","#ddd2c0"},{"@sliderFill","#b47a3d"},{"@handle","#9a5b23"},{"@progress","#b47a3d"},
            {"@roleRef","#f1dfc8"},{"@roleRefText","#5c3612"},{"@roleCand","#e6e0d0"},{"@roleCandText","#4e4636"},
            {"@success","#2a6b3f"},{"@successBg","#e3f0dc"},{"@successBorder","#9cc49a"},
            {"@warning","#7a5410"},{"@warningBg","#f8ebcb"},{"@warningBorder","#d9b56b"},
            {"@danger","#922330"},{"@dangerBg","#f7dfdc"},{"@dangerHover","#f0cbc6"},{"@dangerBorder","#dc9c98"},
            {"@icon","#4a4036"},{"@iconAccent","#9a6a2f"},{"@iconMuted","#9a8c79"},{"@iconActive","#3e8a5a"},{"@keepIcon","#3e8a5a"},{"@removeIcon","#b5505a"},{"@proofText","#2e7a4a"},
            {"@sourceLink","#8a5a20"},{"@thumbHover","#c08a52"},{"@thumbFocus","#7a5a10"},{"@media","#17140f"},{"@mediaFrame","#1d1914"},{"@mediaText","#8a7c69"},{"@thumbBorder","#d6cab6"},{"@grip","#a89a87"},{"@chevron","#4a4036"},{"@thumbSelected","#9a5b23"},{"@thumbMarked","#b5505a"},{"@thumbReference","#3e8a5a"},{"@checkMark","#ffffff"}}}};
    return all;
}
inline const Tokens sizes{{"@fsCaption","11px"},{"@fsBody","13px"},{"@fsTitle","13px"},{"@fsHeading","16px"}};

inline const Info*& active() { static const Info* theme=&themes().front(); return theme; }
inline QString current() { return active()->id; }
inline QColor color(const char* token) {
    for(const auto& [key,value]:active()->tokens) if(qstrcmp(key,token)==0) return QColor(value);
    return QColor(Qt::magenta);
}

// Stylesheet images cannot be recoloured by QSS, so each theme writes its own copies once.
inline QString controlImage(const char* name) {
    static QTemporaryDir directory;
    const auto target=directory.filePath(QString("%1-%2").arg(current(),name));
    if(!QFile::exists(target)) {
        QFile source(QString(":/controls/%1").arg(name)); if(!source.open(QIODevice::ReadOnly)) return {};
        auto svg=source.readAll();
        svg.replace("#e0e9f3",color("@chevron").name().toUtf8()).replace("#eef3fa",color("@checkMark").name().toUtf8()).replace("#5b6f88",color("@grip").name().toUtf8());
        QFile output(target); if(!output.open(QIODevice::WriteOnly) || output.write(svg)!=svg.size()) return QString(":/controls/%1").arg(name);
    }
    return QDir::fromNativeSeparators(target);
}

inline QString styleSheet();
#ifdef Q_OS_MACOS
// macOS alerts show the application's icon at 64 points instead of a generic symbol; warnings keep their sign.
struct MacAlertStyle:QProxyStyle {
    MacAlertStyle():QProxyStyle(QStyleFactory::create("Fusion")) {}
    QIcon standardIcon(StandardPixmap icon,const QStyleOption* option,const QWidget* widget) const override {
        if(icon==SP_MessageBoxInformation || icon==SP_MessageBoxQuestion) {
            static const QIcon application(QCoreApplication::applicationDirPath()+"/../Resources/app.icns");
            if(!application.availableSizes().isEmpty()) return application;
        }
        return QProxyStyle::standardIcon(icon,option,widget);
    }
    int pixelMetric(PixelMetric metric,const QStyleOption* option,const QWidget* widget) const override {
        return metric==PM_MessageBoxIconSize?64:QProxyStyle::pixelMetric(metric,option,widget);
    }
};
#endif
// Combo lists default to a menu-style delegate that ignores ::item padding; the standard item
// delegate draws them as roomy Windows-style rows.
struct ComboListStyle:QObject {
    bool eventFilter(QObject* object,QEvent* event) override {
        if(event->type()==QEvent::Polish) if(auto combo=qobject_cast<QComboBox*>(object); combo && !qobject_cast<QStyledItemDelegate*>(combo->itemDelegate())) combo->setItemDelegate(new QStyledItemDelegate(combo));
        return false;
    }
};
// Unknown ids keep the first (dark) theme so an edited settings file cannot leave the window unstyled.
inline void apply(QApplication& app,const QString& id) {
    active()=&themes().front();
    for(const auto& theme:themes()) if(id==theme.id) active()=&theme;
#ifdef Q_OS_MACOS
    app.setStyle(new MacAlertStyle);
#else
    app.setStyle(QStyleFactory::create("Fusion"));
#endif
    static ComboListStyle comboLists; app.removeEventFilter(&comboLists); app.installEventFilter(&comboLists);
    // Segoe UI is the Windows interface font; elsewhere the system's own (San Francisco on macOS) fits the platform.
#ifdef Q_OS_WIN
    app.setFont(QFont("Segoe UI",9));
#elif defined(Q_OS_MACOS)
    // 12 pt matches the 12 px that Segoe UI 9 pt gives on Windows, the size the layouts are measured for.
    auto font=QFontDatabase::systemFont(QFontDatabase::GeneralFont); font.setPointSize(12); app.setFont(font);
#else
    app.setFont(QFontDatabase::systemFont(QFontDatabase::GeneralFont));
#endif
    QPalette palette;
    palette.setColor(QPalette::Window,color("@surface"));
    palette.setColor(QPalette::WindowText,color("@text"));
    palette.setColor(QPalette::Base,color("@base"));
    palette.setColor(QPalette::AlternateBase,color("@field"));
    palette.setColor(QPalette::Text,color("@text"));
    palette.setColor(QPalette::PlaceholderText,color("@placeholder"));
    palette.setColor(QPalette::Button,color("@control"));
    palette.setColor(QPalette::ButtonText,color("@text"));
    palette.setColor(QPalette::Highlight,color("@highlight"));
    palette.setColor(QPalette::HighlightedText,color("@highlightText"));
    palette.setColor(QPalette::ToolTipBase,color("@tooltip"));
    palette.setColor(QPalette::ToolTipText,color("@tooltipText"));
    palette.setColor(QPalette::Link,color("@link"));
    palette.setColor(QPalette::Disabled,QPalette::Text,color("@faint"));
    palette.setColor(QPalette::Disabled,QPalette::ButtonText,color("@faint"));
    palette.setColor(QPalette::Disabled,QPalette::WindowText,color("@faint"));
    app.setPalette(palette);
    app.setStyleSheet(styleSheet());
#ifdef Q_OS_MACOS
    // The title bar and system controls follow the theme, not the system appearance.
    app.styleHints()->setColorScheme(color("@surface").lightness()<128?Qt::ColorScheme::Dark:Qt::ColorScheme::Light);
#endif
}

inline QString styleSheet() {
    QString style=R"(
        QMainWindow, QDialog { background: @surface; }
        QLabel#secondary, QLabel#metadata, QLabel#cleanupSummaryNote, QLabel#retentionTieBreakerNote, QLabel#settingsNote { color: @muted; }
        QLabel#sectionTitle { font-weight: 600; }
        QLabel#dialogLead { font-size: @fsTitle; font-weight: 600; color: @textStrong; }
        QWidget#playerPanel { background: @raised; border: 1px solid @border; border-radius: 8px; }
        QLabel#panelRole { padding: 1px 7px; border-radius: 4px; font-size: @fsCaption; font-weight: 600; background: @roleRef; color: @roleRefText; }
        QLabel#panelRole[reference="false"] { background: @roleCand; color: @roleCandText; }
        QLineEdit#filename { font-size: @fsTitle; font-weight: 600; color: @textStrong; padding: 3px 2px; background: transparent; border: 0; }
        QLineEdit#folderPath { font-size: @fsCaption; color: @muted; padding: 1px 2px; background: transparent; border: 0; }
        QLabel#timeLabel { color: @soft; font-size: @fsBody; }
        QToolButton { border: 1px solid transparent; border-radius: 5px; padding: 4px; }
        QToolButton:hover { background: @hover; border-color: @hoverBorder; }
        QToolButton:focus { border: 1px solid @focus; background: @focusBg; }
        QToolButton:pressed { background: @pressed; }
        QToolButton:disabled { color: @faint; }
        QToolButton#fileDetails { padding: 1px 6px; }
        QToolButton#primaryButton { background: @accent; border: 1px solid @accentBorder; padding: 4px 12px; font-weight: 600; color: @onAccent; }
        QToolButton#primaryButton:hover { background: @accentHover; }
        QToolButton#sourcesButton, QToolButton#emptySourcesButton, QToolButton#emptyResumeButton { border: 1px solid @borderStrong; background: @control; padding: 4px 12px; }
        QToolButton#sourcesButton:hover, QToolButton#emptySourcesButton:hover, QToolButton#emptyResumeButton:hover { background: @controlHover; }
        QToolButton#emptyScanButton { background: @accent; border: 1px solid @accentBorder; padding: 7px 28px; font-weight: 600; font-size: @fsTitle; color: @onAccent; }
        QToolButton#emptyScanButton:hover { background: @accentHover; }
        QLabel#emptyFieldLabel { color: @muted; }
        QToolButton#stopButton { background: @dangerBg; border: 1px solid @dangerBorder; color: @danger; padding: 4px 10px; font-weight: 600; }
        QToolButton#stopButton:hover { background: @dangerHover; }
        QToolButton#pauseButton { border: 1px solid @borderStrong; background: @control; padding: 4px 10px; }
        QToolButton#pauseButton:hover { background: @controlHover; }
        QToolButton#trashLeft:hover, QToolButton#trashRight:hover { background: @dangerBg; border-color: @dangerBorder; }
        QToolButton#trashLeft:pressed, QToolButton#trashRight:pressed { background: @dangerHover; }
        QToolButton#linkControls:checked { background: @accentSubtle; border: 1px solid @accentLine; color: @accentText; }
        QToolButton#reviewButton { border: 1px solid @borderStrong; background: @control; padding: 4px 10px; }
        QToolButton#reviewButton:hover { background: @controlHover; }
        QToolButton#reviewButton:checked { background: @accentSubtle; border-color: @accentLine; color: @accentText; font-weight: 600; }
        QToolButton#reviewButton:disabled { color: @faint; border-color: @border; background: transparent; }
        QToolButton#decisionButton { border: 1px solid @borderStrong; border-right: 0; border-radius: 0; background: @control; padding: 4px 12px; }
        QToolButton#decisionButton[segment="first"] { border-top-left-radius: 5px; border-bottom-left-radius: 5px; }
        QToolButton#decisionButton[segment="last"] { border-right: 1px solid @borderStrong; border-top-right-radius: 5px; border-bottom-right-radius: 5px; }
        QToolButton#decisionButton:hover { background: @controlHover; }
        QToolButton#decisionButton:checked { background: @accentSubtle; color: @accentText; font-weight: 600; }
        QToolButton#decisionButton:disabled { color: @faint; background: transparent; }
        QFrame#panelSeparator { background: @border; }
        QLabel#pairSimilarity { padding: 4px 10px; border-radius: 11px; border: 1px solid @border; background: @raised; color: @text; }
        QLabel#pairSimilarity[verdict="identical"] { background: @successBg; border-color: @successBorder; color: @success; font-weight: 600; }
        QLabel#pairSimilarity[verdict="similar"] { background: @warningBg; border-color: @warningBorder; color: @warning; font-weight: 600; }
        QLabel#pairSimilarity[verdict="candidate"] { background: @control; border-color: @borderStrong; color: @text; font-weight: 600; }
        QLabel#pairSimilarity[verdict="none"] { background: transparent; border-color: transparent; color: @muted; }
        QLabel#groupPosition { font-weight: 600; }
        QWidget#emptyState { background: transparent; }
        QWidget#emptyCard { background: @raised; border: 1px solid @border; border-radius: 12px; }
        QWidget#emptyDropZone { background: @sunken; border: 1px dashed @borderStrong; border-radius: 8px; }
        QToolButton#emptyLinkButton:hover { background: transparent; border-color: transparent; text-decoration: underline; }
        QLabel#emptyTitle { font-size: @fsHeading; font-weight: 600; color: @textStrong; }
        QLabel#emptyHint { color: @muted; }
        QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox { background: @field; border: 1px solid @fieldBorder; border-radius: 4px; padding: 4px 8px; selection-background-color: @selection; selection-color: @highlightText; }
        QLineEdit:focus, QComboBox:focus { border-color: @fieldFocus; }
        QComboBox { padding-right: 26px; combobox-popup: 0; }
        QComboBox::drop-down { subcontrol-origin: padding; subcontrol-position: center right; width: 22px; border: 0; background: transparent; }
        QComboBox::down-arrow { image: url(@img:chevron-down.svg); width: 10px; height: 7px; }
        QComboBox::down-arrow:disabled { image: none; }
        QComboBox QAbstractItemView { background: @menu; border: 1px solid @borderStrong; selection-background-color: @menuSelected; selection-color: @textStrong; }
        QComboBox QAbstractItemView { padding: 2px; outline: 0; }
        QComboBox QAbstractItemView::item { padding: 5px 8px; border-radius: 3px; }
        QComboBox QAbstractItemView::item:hover, QComboBox QAbstractItemView::item:selected { background: @menuSelected; color: @textStrong; }
        QSpinBox, QDoubleSpinBox { padding-top: 2px; padding-bottom: 2px; padding-right: 26px; }
        QSpinBox#similarityThreshold, QDoubleSpinBox#alignmentOffset { padding: 2px 24px 2px 6px; border-radius: 4px; }
        QSpinBox::up-button, QSpinBox::down-button, QDoubleSpinBox::up-button, QDoubleSpinBox::down-button { subcontrol-origin: padding; width: 20px; background: transparent; border: 0; border-left: 1px solid @fieldBorder; }
        QSpinBox#similarityThreshold::up-button, QSpinBox#similarityThreshold::down-button, QDoubleSpinBox#alignmentOffset::up-button, QDoubleSpinBox#alignmentOffset::down-button { height: 13px; }
        QSpinBox::up-button, QDoubleSpinBox::up-button { subcontrol-position: top right; border-top-right-radius: 3px; }
        QSpinBox::down-button, QDoubleSpinBox::down-button { subcontrol-position: bottom right; border-bottom-right-radius: 3px; }
        QSpinBox::up-button:hover, QSpinBox::down-button:hover, QDoubleSpinBox::up-button:hover, QDoubleSpinBox::down-button:hover { background: @spinHover; }
        QSpinBox::up-button:pressed, QSpinBox::down-button:pressed, QDoubleSpinBox::up-button:pressed, QDoubleSpinBox::down-button:pressed { background: @spinPressed; }
        QSpinBox::up-arrow, QDoubleSpinBox::up-arrow { image: url(@img:chevron-up.svg); width: 10px; height: 6px; }
        QSpinBox::down-arrow, QDoubleSpinBox::down-arrow { image: url(@img:chevron-down.svg); width: 10px; height: 6px; }
        QSpinBox::up-arrow:disabled, QSpinBox::down-arrow:disabled, QDoubleSpinBox::up-arrow:disabled, QDoubleSpinBox::down-arrow:disabled { image: none; }
        QPushButton { padding: 4px 14px; border: 1px solid @borderStrong; border-radius: 5px; background: @control; color: @text; }
        QPushButton:hover { background: @controlHover; }
        QPushButton:default { border-color: @focus; }
        QPushButton:disabled { color: @faint; }
        QPushButton#dangerButton { background: @dangerBg; border: 1px solid @dangerBorder; color: @danger; font-weight: 600; }
        QPushButton#dangerButton:hover { background: @dangerHover; }
        QPushButton#dangerButton:disabled { background: @control; border-color: @border; color: @faint; }
        QTreeWidget, QListWidget { border: 1px solid @border; border-radius: 5px; background: @base; outline: 0; }
        QTreeWidget::item { padding: 2px 4px; border-bottom: 1px solid @rowLine; }
        QTreeWidget::item:selected, QListWidget::item:selected { background: @rowSelected; color: @textStrong; }
        QListWidget::item { padding: 5px 8px; border-radius: 4px; }
        QListWidget::item:hover { background: @hover; }
        QTreeView::indicator, QCheckBox::indicator { width: 13px; height: 13px; border: 1px solid @indicatorBorder; border-radius: 3px; background: @field; }
        QTreeView::indicator:checked, QCheckBox::indicator:checked { background: @accent; border-color: @accentBorder; image: url(@img:check-on.svg); }
        QTreeView::indicator:hover, QCheckBox::indicator:hover { border-color: @focus; }
        QCheckBox::indicator:disabled { border-color: @border; background: transparent; }
        QCheckBox::indicator:checked:disabled { background: @control; }
        QHeaderView::section { background: @header; color: @headerText; padding: 5px 10px; border: 0; border-bottom: 1px solid @fieldBorder; font-size: @fsCaption; }
        QMenuBar { background: @surface; padding: 1px 6px; }
        QMenuBar::item { padding: 4px 9px; }
        QMenuBar::item:selected, QMenu::item:selected { background: @menuSelected; color: @textStrong; }
        QMenu { background: @menu; border: 1px solid @borderStrong; padding: 4px; }
        QMenu::item { padding: 4px 24px 4px 10px; }
        QMenu::item:disabled { color: @faint; }
        QMenu::separator { height: 1px; background: @border; margin: 4px 8px; }
        QStatusBar { color: @statusText; border-top: 1px solid @statusLine; padding: 1px 10px; }
        QSlider::groove:horizontal { background: @groove; height: 4px; border-radius: 2px; }
        QSlider::sub-page:horizontal { background: @sliderFill; border-radius: 2px; }
        QSlider::handle:horizontal { background: @handle; width: 10px; margin: -4px 0; border-radius: 5px; }
        QSplitter::handle:vertical { background: transparent; image: url(@img:grip-horizontal.svg); }
        QSplitter::handle:vertical:hover { background: @raised; border-radius: 4px; }
        QProgressBar { border: 0; background: @control; }
        QProgressBar::chunk { background: @progress; }
        QGroupBox { border: 1px solid @border; border-radius: 6px; margin-top: 14px; padding: 12px 10px 8px 10px; font-weight: 600; }
        QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; color: @muted; }
        QToolTip { padding: 5px; border: 1px solid @tooltipBorder; background: @tooltip; color: @tooltipText; }
    )";
    for(const auto* name:{"chevron-down.svg","chevron-up.svg","check-on.svg","grip-horizontal.svg"}) style.replace(QString("@img:")+name,controlImage(name));
    // Longest names first so "@dangerBg" is not consumed by "@danger".
    std::vector<std::pair<QString,QString>> ordered;
    for(const auto& [key,value]:active()->tokens) ordered.emplace_back(key,value);
    for(const auto& [key,value]:sizes) ordered.emplace_back(key,value);
    std::sort(ordered.begin(),ordered.end(),[](const auto& a,const auto& b) { return a.first.size()>b.first.size(); });
    for(const auto& [key,value]:ordered) style.replace(key,value);
    return style;
}
}
