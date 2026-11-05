#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "Language.h"
#include <QDateTime>
#include <QDesktopServices>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>
#include <tuple>

using Language::text;

namespace {
// Releases are tagged v<major>.<minor>.<patch>; a preview or development build counts as older than the release
// with the same number, so testers are offered the final version.
struct ReleaseVersion { int major=0,minor=0,patch=0; bool preview=false,valid=false; };
ReleaseVersion parseVersion(const QString& value) {
    const auto match=QRegularExpression("^v?(\\d+)\\.(\\d+)\\.(\\d+)(-.+)?$").match(value.trimmed());
    if(!match.hasMatch()) return {};
    return {match.captured(1).toInt(),match.captured(2).toInt(),match.captured(3).toInt(),!match.captured(4).isEmpty(),true};
}
bool newer(const ReleaseVersion& latest,const ReleaseVersion& current) {
    if(!latest.valid || !current.valid || latest.preview) return false;
    const auto a=std::tie(latest.major,latest.minor,latest.patch),b=std::tie(current.major,current.minor,current.patch);
    return a>b || (a==b && current.preview);
}
}

// Flathub delivers updates to the Flatpak itself, so the app does not look for them there.
bool MainWindow::updateChecksAvailable() const { return !QFile::exists("/.flatpak-info"); }

void MainWindow::startUpdateChecks() {
    if(!updateChecksAvailable()) return;
    // A few seconds after start, so the window is up first. The app asks once before it ever goes online, as HandBrake does.
    QTimer::singleShot(3000,this,[this] {
        if(closing_) return;
        if(!settings_.contains("updates/check")) {
            QMessageBox ask(QMessageBox::Question,text("Check for updates"),text("Would you like Compareel to check for updates automatically?"),QMessageBox::Yes|QMessageBox::No,this);
            ask.setInformativeText(text("You can change this later in Settings.")); ask.setDefaultButton(QMessageBox::Yes);
            settings_.setValue("updates/check",ask.exec()==QMessageBox::Yes); settings_.sync();
        }
        if(!settings_.value("updates/check").toBool()) return;
        const auto last=settings_.value("updates/last").toDateTime();
        if(last.isValid() && last.secsTo(QDateTime::currentDateTimeUtc())<24*3600) return;
        checkForUpdates(false);
    });
}

void MainWindow::checkForUpdates(bool manual) {
    if(!network_) network_=new QNetworkAccessManager(this);
    QNetworkRequest request(QUrl(QString(projectUrl).replace("https://github.com/","https://api.github.com/repos/")+"/releases/latest"));
    request.setHeader(QNetworkRequest::UserAgentHeader,"Compareel/"+QCoreApplication::applicationVersion());
    request.setRawHeader("Accept","application/vnd.github+json");
    request.setTransferTimeout(15000);
    auto reply=network_->get(request);
    connect(reply,&QNetworkReply::finished,this,[this,reply,manual] {
        reply->deleteLater();
        if(closing_) return;
        const auto release=QJsonDocument::fromJson(reply->readAll()).object();
        const auto tag=release.value("tag_name").toString();
        if(reply->error()!=QNetworkReply::NoError || tag.isEmpty()) {
            if(manual) QMessageBox::warning(this,text("Check for updates"),text("Compareel could not reach GitHub to check for updates. Try again later."));
            return;
        }
        const auto current=QCoreApplication::applicationVersion();
        if(!newer(parseVersion(tag),parseVersion(current))) {
            settings_.setValue("updates/last",QDateTime::currentDateTimeUtc());
            if(manual) QMessageBox::information(this,text("Check for updates"),text("Compareel %1 is the latest version.").arg(current));
            return;
        }
        // An automatic check never interrupts work and reports each version once; the menu command always shows it.
        const auto version=tag.startsWith('v')?tag.mid(1):tag;
        if(!manual && (settings_.value("updates/notified").toString()==version || scanning_ || fileOperationPending_ || QApplication::activeModalWidget())) return;
        settings_.setValue("updates/last",QDateTime::currentDateTimeUtc()); settings_.setValue("updates/notified",version);
        QMessageBox box(QMessageBox::Information,text("Check for updates"),text("Compareel %1 is available. You have %2.").arg(version,current),QMessageBox::NoButton,this);
        auto download=box.addButton(text("Download"),QMessageBox::AcceptRole); box.addButton(text("Later"),QMessageBox::RejectRole);
        box.setDefaultButton(download); box.exec();
        if(box.clickedButton()==download) QDesktopServices::openUrl(QUrl(release.value("html_url").toString(QString(projectUrl)+"/releases/latest")));
    });
}
