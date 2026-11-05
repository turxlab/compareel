#pragma once
#include "platform/Platform.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>

namespace ShellIntegration {
inline constexpr auto Marker=L"Compareel SendTo integration v1";
inline QString shortcutPath() {
    const auto folder=Platform::SendToFolderPath();
    if(folder.empty()) return {};
    return QDir(QString::fromStdWString(folder)).filePath("Compareel.lnk");
}
inline bool owned(const QString& path) {
    return Platform::IsOwnedShortcut(path.toStdWString(),Marker);
}
inline bool install(const QString& path,const QString& executable) {
    if(path.isEmpty() || executable.isEmpty()) return false;
    return Platform::InstallOwnedShortcut(
        path.toStdWString(),
        QDir::toNativeSeparators(path).toStdWString(),
        QDir::toNativeSeparators(executable).toStdWString(),
        L"--",
        Marker);
}
inline bool uninstall(const QString& path) { return owned(path) && QFile::remove(path); }
inline void refresh() {
    const auto path=shortcutPath(); if(owned(path)) install(path,QCoreApplication::applicationFilePath());
}
}
