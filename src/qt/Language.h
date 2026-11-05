#pragma once
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QLocale>
#include <QStandardPaths>
#include <QString>
#include <algorithm>
#include "platform/Platform.h"

// Every user-visible string is looked up by its English wording. A language is one JSON file
// ({"language": "Türkçe", "code": "tr", "strings": {"English": "Translation", ...}}); bundled files
// live in :/languages, and files in the user's "languages" folder (see userFolder) add or override them.
// A missing entry falls back to English, so a partial translation is still usable.
namespace Language {
struct Info { QString code, name, path; };
// The pseudo-code a user picks to follow Windows' display language.
inline const QString systemCode="system";

// The language of a profile that has not chosen one: the system's, or English while self-checks run.
inline QString& defaultCode() { static QString code=systemCode; return code; }
inline QString& currentCode() { static QString code="en"; return code; }
inline QString& selectedCode() { static QString code="en"; return code; }
inline QHash<QString,QString>& table() { static QHash<QString,QString> strings; return strings; }

// Where a user adds or corrects a translation: next to Compareel.exe for the portable Windows package, otherwise
// (a macOS bundle, which must stay unchanged, or an installed Linux program) in the per-user data folder.
inline QString userFolder() {
    const auto programFolder=QCoreApplication::applicationDirPath();
    if(QFileInfo(programFolder).isWritable() && !programFolder.endsWith("/Contents/MacOS")) return QDir(programFolder).filePath("languages");
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath("languages");
}

inline QList<Info> available() {
    QList<Info> result{{"en","English",{}}};
    auto add=[&result](const QString& folder) {
        for(const auto& name:QDir(folder).entryList({"*.json"},QDir::Files,QDir::Name)) {
            const auto path=QDir(folder).filePath(name); QFile file(path);
            if(!file.open(QIODevice::ReadOnly)) continue;
            const auto root=QJsonDocument::fromJson(file.read(16*1024*1024)).object();
            const auto code=root.value("code").toString().trimmed(); auto title=root.value("language").toString().trimmed();
            if(code.isEmpty() || !root.value("strings").isObject()) continue;
            if(title.isEmpty()) title=code;
            auto existing=std::find_if(result.begin(),result.end(),[&code](const Info& info) { return info.code==code; });
            if(existing!=result.end()) *existing={code,title,path}; else result.push_back({code,title,path});
        }
    };
    add(":/languages");
    add(userFolder());
    return result;
}
// The menu entry, as HandBrake shows it: English name, then the language's own name.
inline QString displayName(const Info& info) {
    const auto english=QLocale::languageToString(QLocale(info.code).language());
    if(info.code=="en" || english.isEmpty() || english==info.name || QLocale(info.code).language()==QLocale::C) return info.name;
    return english+" ("+info.name+")";
}
// The language actually shown; selected() is what the user chose, which may be systemCode.
inline QString current() { return currentCode(); }
inline QString selected() { return selectedCode(); }
// Picks the first Windows display language that has a file: exact region or script first
// (pt-BR, zh-TW, sr-Latn), then the bare language, then any region of it (pt-PT finds pt-BR).
inline QString systemLanguage(const QList<Info>& languages) {
    auto find=[&languages](const QString& code) { for(const auto& info:languages) if(info.code.compare(code,Qt::CaseInsensitive)==0) return info.code; return QString(); };
    for(const auto& name:QLocale::system().uiLanguages()) {
        const QLocale locale(name); const auto language=QLocale::languageToCode(locale.language());
        if(language.isEmpty() || language=="C") continue;
        QStringList candidates;
        if(language=="zh") {
            const bool traditional=locale.script()==QLocale::TraditionalChineseScript || (locale.script()==QLocale::AnyScript && QStringList{"TW","HK","MO"}.contains(QLocale::territoryToCode(locale.territory())));
            candidates<<(traditional?"zh-TW":"zh-CN");
        }
        if(locale.script()==QLocale::LatinScript) candidates<<language+"-Latn";
        candidates<<language+"-"+QLocale::territoryToCode(locale.territory())<<language;
        for(const auto& code:candidates) if(const auto match=find(code); !match.isEmpty()) return match;
        for(const auto& info:languages) if(info.code.startsWith(language+"-",Qt::CaseInsensitive)) return info.code;
    }
    return "en";
}
// Unknown codes fall back to English; returns whether the requested language was found.
// Arabic, Persian and other right-to-left languages mirror the whole interface.
inline bool setCurrent(const QString& code) {
    table().clear(); currentCode()="en"; selectedCode()=code;
    const auto languages=available();
    const auto wanted=code==systemCode?systemLanguage(languages):code;
    bool found=false;
    for(const auto& info:languages) {
        if(info.code!=wanted) continue;
        currentCode()=wanted; found=true;
        if(info.path.isEmpty()) break;
        QFile file(info.path); if(!file.open(QIODevice::ReadOnly)) { currentCode()="en"; found=false; break; }
        const auto strings=QJsonDocument::fromJson(file.read(16*1024*1024)).object().value("strings").toObject();
        for(auto it=strings.begin();it!=strings.end();++it) if(it.value().isString()) table().insert(it.key(),it.value().toString());
        break;
    }
    if(!found && code!=systemCode) selectedCode()="en";
    if(qGuiApp) QGuiApplication::setLayoutDirection(QLocale(currentCode()).textDirection());
    return found || code==systemCode;
}
// Keys are written with Windows' names; on macOS and Linux the same key with "Trash" and "Finder" or
// "file manager" is looked up first, so each platform reads its own words. A language that has not translated
// that variant yet keeps its Windows wording rather than falling back to English.
inline QString platformWording(QString english) {
    static const auto trash=QString::fromWCharArray(Platform::TrashName()), manager=QString::fromWCharArray(Platform::FileManagerName());
    if(trash!="Recycle Bin") english.replace("Recycle Bin",trash);
    if(manager!="Explorer") english.replace("Explorer",manager);
    return english;
}
inline QString text(const char* key) {
    const auto english=QString::fromUtf8(key), local=platformWording(english);
    if(local!=english) if(auto found=table().constFind(local); found!=table().cend()) return *found;
    if(auto found=table().constFind(english); found!=table().cend()) return *found;
    return local;
}
}
