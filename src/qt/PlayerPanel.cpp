#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "Language.h"
#include "Icons.h"
#include "FileOperations.h"
#include "ScanCache.h"
#include "ScanManifest.h"
#include "Csv.h"
#include "SubtitleInfo.h"
#include "SeekSlider.h"
#include "ImageReader.h"
#include "ShellIntegration.h"
#include "FFmpegReader.h"
#include "ScanConcurrency.h"
#include "platform/Platform.h"
#include <QApplication>
#include <QActionGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QContextMenuEvent>
#include "NativePath.h"
#include <QDesktopServices>
#include <QDebug>
#include <QDialogButtonBox>
#include <QDir>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QFocusEvent>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGridLayout>
#include <QHeaderView>
#include <QLabel>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QGroupBox>
#include <QMenuBar>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QProgressDialog>
#include <QProcess>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QScrollArea>
#include <QScreen>
#include <QSqlQuery>
#include <QWindow>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QStyleOptionSlider>
#include <QSplitter>
#include <QStandardPaths>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QThread>
#include <QToolButton>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <cmath>

using Language::text;

VideoCanvas::VideoCanvas(VideoPlayer* player, QWidget* parent) : QWidget(parent), player_(player) {
    setMinimumSize(0,48);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setToolTip(text("Hold B on a preview to show the other file; release to return."));
    setAccessibleName(text("Comparison"));
}
void VideoCanvas::setTransform(double zoom, QPointF pan) { zoom_=zoom; pan_=pan; update(); }
void VideoCanvas::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), Theme::color("@media"));
    if(previewHidden_) {
        icon(Glyph::EyeOff,QColor("#7e91a4")).paint(&painter,QRect(rect().center()-QPoint(18,18),QSize(36,36))); return;
    }
    auto frame = showingReference_ && reference_ && !reference_->previewHidden_ ? reference_->frame() : this->frame();
    if (!frame) {
        painter.setPen(Theme::color("@mediaText"));
        painter.drawText(rect().adjusted(20,20,-20,-20), Qt::AlignCenter,
            still_?text(stillFailed_?"Preview unavailable":"Loading image…"):player_->GetFilePath().empty() ? text("No file selected") :
            FAILED(player_->GetError()) ? text("Preview unavailable") : text("Loading preview…"));
        return;
    }
    painter.setRenderHint(QPainter::SmoothPixmapTransform,zoom_ <= 1.0);
    auto draw=[&](const std::shared_ptr<const VideoFrame>& value) {
        QImage image(value->pixels.data(),value->width,value->height,value->width*4,QImage::Format_RGB32);
        QSizeF size=image.size(); size.scale(this->size(),Qt::KeepAspectRatio); size*=zoom_;
        painter.drawImage(QRectF(QPointF((width()-size.width())/2,(height()-size.height())/2)+pan_,size),image);
    };
    auto reference=reference_?reference_->frame():nullptr;
    if(showingReference_ || !mode_ || !reference) { draw(frame); return; }
    draw(reference);
    painter.save();
    if(mode_==1) painter.setClipRect(QRectF(width()*wipe_,0,width()*(1-wipe_),height()));
    if(mode_==2) painter.setOpacity(0.5);
    if(mode_==3) painter.setCompositionMode(QPainter::CompositionMode_Difference);
    draw(frame); painter.restore();
    if(mode_==1) { painter.setPen(QPen(QColor("#9cd4e1"),2)); painter.drawLine(QPointF(width()*wipe_,0),QPointF(width()*wipe_,height())); }
    QString label=text(mode_==1?"Wipe":mode_==2?"Overlay":"Difference (display aid)");
    QRect badge=painter.fontMetrics().boundingRect(label).adjusted(-8,-4,8,4); badge.moveTopLeft(QPoint(10,10));
    painter.fillRect(badge,QColor(15,23,33,220)); painter.setPen(QColor("#dce8f2")); painter.drawText(badge,Qt::AlignCenter,label);
}
void VideoCanvas::wheelEvent(QWheelEvent* event) {
    zoom_=std::clamp(zoom_*(event->angleDelta().y()>0?1.15:1/1.15),0.5,8.0);
    if (transformChanged) transformChanged(zoom_,pan_);
    update(); event->accept();
}
void VideoCanvas::keyPressEvent(QKeyEvent* event) {
    if(event->key()==Qt::Key_B && event->modifiers()==Qt::NoModifier && reference_) {
        if(!event->isAutoRepeat()) showingReference_=!previewHidden_ && !reference_->previewHidden_ && bool(frame()) && bool(reference_->frame());
        update(); event->accept(); return;
    }
    QWidget::keyPressEvent(event);
}
void VideoCanvas::keyReleaseEvent(QKeyEvent* event) {
    if(event->key()==Qt::Key_B) {
        if(!event->isAutoRepeat()) { showingReference_=false; update(); }
        event->accept(); return;
    }
    QWidget::keyReleaseEvent(event);
}
void VideoCanvas::focusOutEvent(QFocusEvent* event) { if(animationHeld_ && animationHold) animationHold(false); animationHeld_=false; showingReference_=false; update(); QWidget::focusOutEvent(event); }
void VideoCanvas::mousePressEvent(QMouseEvent* event) {
    setFocus(Qt::MouseFocusReason);
    // Only the left button drags; a right click opens the context menu without moving the wipe line or grabbing the image.
    if(event->button()!=Qt::LeftButton) { QWidget::mousePressEvent(event); return; }
    // Press-and-hold plays an animation only when the press has nothing else to do: no wipe line to drag, nothing zoomed to pan.
    if(mode_!=1 && zoom_<=1 && animationHold && animationHold(true)) { animationHeld_=true; event->accept(); return; }
    if(mode_==1) { wipe_=std::clamp(event->position().x()/std::max(1,width()),0.0,1.0); update(); return; }
    dragStart_=event->position(); setCursor(Qt::ClosedHandCursor);
}
void VideoCanvas::mouseMoveEvent(QMouseEvent* event) {
    if(animationHeld_) { event->accept(); return; }
    if (event->buttons() & Qt::LeftButton) {
        if(mode_==1) { wipe_=std::clamp(event->position().x()/std::max(1,width()),0.0,1.0); update(); return; }
        pan_ += event->position()-dragStart_; dragStart_=event->position();
        if (transformChanged) transformChanged(zoom_,pan_);
        update();
    } else setCursor(zoom_>1?Qt::OpenHandCursor:Qt::ArrowCursor);
}
void VideoCanvas::mouseReleaseEvent(QMouseEvent* event) {
    if(event->button()==Qt::LeftButton && animationHeld_) { animationHeld_=false; if(animationHold) animationHold(false); event->accept(); return; }
    QWidget::mouseReleaseEvent(event);
}
void VideoCanvas::mouseDoubleClickEvent(QMouseEvent* event) {
    if(event->button()!=Qt::LeftButton) { QWidget::mouseDoubleClickEvent(event); return; }
    if(zoom_<=1 && mode_!=1 && animationHold && animationHold(true)) { animationHeld_=true; event->accept(); return; }
    setTransform(1,{}); if(transformChanged) transformChanged(1,{});
}

PlayerPanel::PlayerPanel(bool reference,QThreadPool* imagePool,QWidget* parent) : QWidget(parent), reference_(reference),imagePool_(imagePool) {
    setObjectName("playerPanel");
    auto layout = new QVBoxLayout(this); layout->setContentsMargins(10,10,10,10); layout->setSpacing(6);
    layout->setSizeConstraint(QLayout::SetMinimumSize);
    auto heading = new QHBoxLayout; heading->setSpacing(6);
    // The role caption answers "which side is which" before the file name does.
    role_=new QLabel(this); role_->setObjectName("panelRole"); role_->setProperty("reference",reference); heading->addWidget(role_);
    name_=new ElidedLineEdit(this); name_->setObjectName("filename"); name_->setMinimumWidth(0); name_->setFrame(false);
    heading->addWidget(name_,1);
    protect_=tool(this,Glyph::Unlock,"Protect this file",[this] { if(protectRequested) protectRequested(); });
    location_=tool(this,Glyph::Folder,"Show in Explorer",[this] {
        QString path=QDir::toNativeSeparators(path_);
        if(!Platform::RevealInFileManager(path.toStdWString())) {
            if(QFileInfo::exists(path_)) QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path_).absolutePath()));
            else QMessageBox::information(this,text("Show in Explorer"),text("This file is no longer available."));
        }
    });
    remove_=tool(this,Glyph::Trash,"Move to Recycle Bin",[this] { if(removeRequested) removeRequested(); });
    remove_->setObjectName(reference?"trashLeft":"trashRight"); remove_->style()->unpolish(remove_); remove_->style()->polish(remove_);
    // Keep the destructive action visually apart from the harmless ones.
    auto separator=new QFrame(this); separator->setObjectName("panelSeparator"); separator->setFixedSize(1,20);
    heading->addWidget(protect_); heading->addWidget(location_); heading->addSpacing(6); heading->addWidget(separator); heading->addSpacing(6); heading->addWidget(remove_);
    layout->addLayout(heading);

    canvas=new VideoCanvas(player.get(),this); layout->addWidget(canvas,1);
    for(QWidget* surface:{static_cast<QWidget*>(name_),static_cast<QWidget*>(canvas)}) {
        surface->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(surface,&QWidget::customContextMenuRequested,this,[this,surface](QPoint point) {
            if(fileMenuRequested) fileMenuRequested(surface->mapToGlobal(point));
        });
    }
    transport_=new QWidget(this); auto transport = new QHBoxLayout(transport_); transport->setContentsMargins(0,0,0,0); transport->setSpacing(4);
    play_=tool(this,Glyph::Play,"Play / pause",[this] { if(playRequested) playRequested(); else if(still_) playImage(!imagePlaying_); else player->TogglePlayPause(); refresh(); });
    previous_=tool(this,Glyph::Previous,"Previous frame",[this] { if(still_) { if(imageStepRequested) imageStepRequested(false); else stepImage(false); } else if(stepRequested) stepRequested(false); else player->StepFrame(false); });
    next_=tool(this,Glyph::Next,"Next frame",[this] { if(still_) { if(imageStepRequested) imageStepRequested(true); else stepImage(true); } else if(stepRequested) stepRequested(true); else player->StepFrame(true); });
    transport->addWidget(play_); transport->addWidget(previous_); transport->addWidget(next_);
    seek_=new SeekSlider(this); seek_->setObjectName("playerSeek"); seek_->setRange(0,10000); seek_->setAccessibleName(text("Duration")); transport->addWidget(seek_,1);
    time_=new QLabel("00:00.0 / 00:00.0",this); time_->setObjectName("timeLabel"); time_->setSizePolicy(QSizePolicy::Maximum,QSizePolicy::Preferred); transport->addWidget(time_);
    mute_=tool(this,Glyph::Volume,"Mute / unmute",[this] { player->SetMuted(!player->IsMuted()); refresh(); }); transport->addWidget(mute_);
    layout->addWidget(transport_); transport_->setObjectName("playerTransport");
    QObject::connect(seek_,&QSlider::sliderMoved,this,[this](int value) {
        if(still_) { if(imageSeekRequested) imageSeekRequested(value); return; }
        if(seekRequested) seekRequested(player->GetDuration100ns()*value/10000,false); else player->ScrubRatio(value/10000.0);
        auto total=player->GetDuration100ns();
        time_->setText(duration(total*value/10000)+" / "+duration(total));
    });
    QObject::connect(seek_,&QSlider::sliderReleased,this,[this] { if(still_) return; if(seekRequested) seekRequested(player->GetDuration100ns()*seek_->value()/10000,true); else player->SeekRatio(seek_->value()/10000.0); });
    QObject::connect(seek_,&QSlider::valueChanged,this,[this](int value) {
        if(still_) { if(imageSeekRequested) imageSeekRequested(value); return; }
        if(!seek_->isSliderDown()) { if(seekRequested) seekRequested(player->GetDuration100ns()*value/10000,true); else player->SeekRatio(value/10000.0); }
    });

    auto details=new QWidget(this); details->setObjectName("playerDetails");
    auto detailLayout=new QVBoxLayout(details); detailLayout->setContentsMargins(0,0,0,0); detailLayout->setSpacing(4);
    auto folderRow=new QHBoxLayout; folderRow->setSpacing(6);
    folderTitle_=new QLabel(this); folderTitle_->setObjectName("secondary");
    pathLabel_=new ElidedLineEdit(this); pathLabel_->setObjectName("folderPath"); pathLabel_->setMinimumWidth(0); pathLabel_->setFrame(false);
    folderRow->addWidget(folderTitle_); folderRow->addWidget(pathLabel_,1); detailLayout->addLayout(folderRow);
    metadata_=new QLabel(this); metadata_->setObjectName("metadata");
    metadata_->setTextInteractionFlags(Qt::TextSelectableByMouse); metadata_->setTextFormat(Qt::RichText);
    auto metadataRow=new QHBoxLayout; metadataRow->setContentsMargins(0,0,0,0); metadataRow->setSpacing(4); metadataRow->addWidget(metadata_,1);
    frameCount_=new QLabel(this); frameCount_->setObjectName("animationFrameCount"); frameCount_->hide(); metadataRow->addWidget(frameCount_);
    details_=new QToolButton(this); details_->setObjectName("fileDetails"); metadataRow->addWidget(details_);
    connect(details_,&QToolButton::clicked,this,[this] { if(detailsRequested) detailsRequested(); }); detailLayout->addLayout(metadataRow);
    layout->addWidget(details);
    QWidget::setTabOrder(name_,protect_); QWidget::setTabOrder(protect_,location_); QWidget::setTabOrder(location_,remove_);
    QWidget::setTabOrder(remove_,play_); QWidget::setTabOrder(play_,previous_); QWidget::setTabOrder(previous_,next_);
    QWidget::setTabOrder(next_,seek_); QWidget::setTabOrder(seek_,mute_); QWidget::setTabOrder(mute_,pathLabel_); QWidget::setTabOrder(pathLabel_,details_);
    player->SetMuted(!reference);
    setFile(nullptr,-1); setCandidates(nullptr); translate();
}
void PlayerPanel::updateRole() {
    // In a group of three or more, the candidate caption also says which of the candidates is shown.
    role_->setText(reference_?text("Reference"):activeGroupTotal_>2?text("Candidate %1/%2").arg(activeRightSlot_).arg(activeGroupTotal_-1):text("Candidate"));
    role_->setToolTip(reference_?text("Reference · kept by default"):text("Candidate · compared with the reference"));
}
void PlayerPanel::translate() {
    updateRole();
    canvas->setAccessibleName(reference_ ? text("Left (#1)") :
        (activeGroupTotal_>=2 ? text("Right (#%1 of %2)").arg(activeRightSlot_+1).arg(activeGroupTotal_) : text("Right (#2)")));
    folderTitle_->setText(text("Folder"));
    details_->setText(text("Details…")); details_->setToolTip(text("File details")); details_->setAccessibleName(text("File details"));
    name_->setAccessibleName(text("File name"));
    pathLabel_->setAccessibleName(text("Folder"));
    seek_->setAccessibleName(text("Duration")); seek_->setToolTip(text("Duration"));
    for(auto [button,key] : std::initializer_list<std::pair<QToolButton*,const char*>>{
        {play_,"Play / pause"},{previous_,"Previous frame"},{next_,"Next frame"},{mute_,"Mute / unmute"},
        {protect_,"Protect this file"},{location_,"Show in Explorer"},{remove_,"Move to Recycle Bin"}}) {
        button->setToolTip(text(key)); button->setAccessibleName(text(key));
    }
    if(fileIndex<0) { name_->setFullText(text("No file selected")); pathLabel_->setFullText({}); }
}
void PlayerPanel::setCandidates(const VideoGroup* group) {
    activeGroupTotal_=group?static_cast<int>(group->fileIndices.size()):0;
    activeRightSlot_=activeGroupTotal_>=2?std::clamp(group->rightSlot,1,activeGroupTotal_-1):1;
    updateRole();
    canvas->setAccessibleName(reference_?text("Left (#1)"):(activeGroupTotal_>=2?
        text("Right (#%1 of %2)").arg(activeRightSlot_+1).arg(activeGroupTotal_):text("Right (#2)")));
}
void PlayerPanel::setFile(const VideoFileInfo* file, int index) {
    fileIndex=index;
    QString path=file?QString::fromStdWString(file->fullPath):QString();
    if(path!=path_) {
        imagePlaying_=false; imageSequence_={}; imageTime_=0; imageFrameIndex_=0;
        auto old=detachPlayer();
        path_=path; fileIndex=index;
        if(file && StillImages::supported(file->fullPath)) {
            setMediaMode(true); canvas->setStillFrame({},true); player->ClosePrevious(std::move(old));
            imageSource_=*file;
            auto watcher=new QFutureWatcher<StillImages::ImageExtendedInfo>(this); const auto revision=imageRevision_;
            auto clock=std::make_shared<QElapsedTimer>(); clock->start(); const auto name=QString::fromStdWString(file->fileName);
            trace(qPrintable("panel image queued: "+name+QString(" (pool busy %1/%2)").arg(imagePool_->activeThreadCount()).arg(imagePool_->maxThreadCount())),*clock);
            connect(watcher,&QFutureWatcherBase::finished,this,[this,watcher,revision,clock,name] {
                auto result=watcher->result(); watcher->deleteLater();
                trace(qPrintable("panel image decoded: "+name),*clock);
                if(revision!=imageRevision_) return;
                imageSequence_=std::move(result);
                const bool steppable=imageDocument() || animationReady();
                transport_->setVisible(steppable); mute_->setVisible(false); play_->setVisible(animationReady());
                previous_->setEnabled(steppable); next_->setEnabled(steppable);
                { QSignalBlocker blocker(seek_); seek_->setRange(0,imageDocument()?int(imageSequence_.frameOrPageCount)-1:10000); }
                showImageIndex(0); refresh(); trace(qPrintable("panel image shown: "+name),*clock);
            });
            watcher->setFuture(QtConcurrent::run(imagePool_,[source=*file,clock,name]() -> StillImages::ImageExtendedInfo {
                trace(qPrintable("panel image decoding started: "+name),*clock);
                try {
                    StillImages::ImageExtendedInfo result; result.keepPlaybackFrames=true;
                    std::atomic<bool> cancel{false}; HRESULT error=S_OK; const auto image=StillImages::read(source,cancel,error,nullptr,true,&result);
                    if(image.isNull()) return {};
                    if(result.playbackFrames.empty()) result.playbackFrames.push_back(image);
                    // Animation frames stay decoded for the whole comparison, so they are kept at screen size and within
                    // about 384 MiB per panel; a long animation no longer holds gigabytes. Still images keep full resolution.
                    if(result.kind==ImageEvidenceKind::AnimatedImage && result.playbackFrames.size()>1) {
                        constexpr double budgetPixels=96.0*1024*1024;
                        double total=0; QSize largest;
                        for(const auto& frame:result.playbackFrames) { total+=double(frame.width())*frame.height(); largest=largest.expandedTo(frame.size()); }
                        double scale=std::min(1.0,std::sqrt(budgetPixels/std::max(1.0,total)));
                        if(largest.width()>0 && largest.height()>0) scale=std::min({scale,2560.0/largest.width(),1440.0/largest.height()});
                        if(scale<1.0) for(auto& frame:result.playbackFrames)
                            frame=frame.scaled(std::max(1,int(frame.width()*scale)),std::max(1,int(frame.height()*scale)),Qt::IgnoreAspectRatio,Qt::SmoothTransformation);
                    }
                    return result;
                } catch(...) { return {}; }
            }));
        } else if(file) { setMediaMode(false); mute_->show(); play_->show(); { QSignalBlocker blocker(seek_); seek_->setRange(0,10000); } player->Load(file->fullPath,std::move(old)); }
        else player->ClosePrevious(std::move(old));
        canvas->setTransform(1,{});
    }
    QString fileName=file?QString::fromStdWString(file->fileName):text("No file selected");
    QString folder=file?QDir::toNativeSeparators(file->folderPath.empty()?QFileInfo(path).absolutePath():QString::fromStdWString(file->folderPath)):QString();
    name_->setFullText(fileName); pathLabel_->setFullText(folder);
    name_->setToolTip(path); pathLabel_->setToolTip(folder);
    self_=file?PeerInfo{true,file->fileSize,file->duration100ns,file->width,file->height,file->codec}:PeerInfo{};
    if(!file) peer_={};
    updateMetadata();
    details_->setEnabled(file!=nullptr);
    protect_->setIcon(icon(file && file->protectedFile?Glyph::Lock:Glyph::Unlock,file && file->protectedFile?"@iconActive":"@iconMuted"));
    protect_->setEnabled(file && !file->protectedByFolder); remove_->setEnabled(file && !file->protectedFile); location_->setEnabled(file!=nullptr);
    refresh();
}
void PlayerPanel::setPeer(const VideoFileInfo* other) {
    peer_=other && self_.valid?PeerInfo{true,other->fileSize,other->duration100ns,other->width,other->height,other->codec}:PeerInfo{};
    updateMetadata();
}
void PlayerPanel::updateMetadata() {
    if(!self_.valid) { metadata_->setText(QString("—   ·   —   ·   —")); metadata_->setToolTip({}); return; }
    // Values that differ from the other file are highlighted so the pair can be compared at a glance.
    auto part=[warning=Theme::color("@warning").name()](const QString& value,bool differs) {
        return differs?QString("<span style=\"color:%2;font-weight:600\">%1</span>").arg(value.toHtmlEscaped(),warning):value.toHtmlEscaped();
    };
    const bool compare=peer_.valid;
    // Compare displayed values, so two files that both read "190.2 KB" are not flagged.
    const auto size=QString::fromStdWString(FormatBytesShort(self_.size));
    QStringList parts{part(size,compare && QString::fromStdWString(FormatBytesShort(peer_.size))!=size)};
    if(!still_) parts<<part(duration(self_.duration),compare && duration(peer_.duration)!=duration(self_.duration));
    parts<<part(QString("%1 × %2").arg(self_.width).arg(self_.height),compare && (peer_.width!=self_.width || peer_.height!=self_.height));
    if(!self_.codec.empty()) parts<<part(QString::fromStdWString(self_.codec),compare && peer_.codec!=self_.codec);
    metadata_->setText(parts.join("&nbsp;&nbsp;&nbsp;·&nbsp;&nbsp;&nbsp;"));
    metadata_->setToolTip(compare?text("Highlighted values differ from the other file."):QString());
}
void PlayerPanel::setRemovalEnabled(bool enabled,const QString& explanation) {
    remove_->setEnabled(enabled); remove_->setToolTip(explanation.isEmpty()?text(enabled?"Move to Recycle Bin":"Protected"):explanation);
}
void PlayerPanel::refresh() {
    frameCount_->setVisible(still_ && animationReady());
    frameCount_->setText(animationReady()?text("%1 frames").arg(imageSequence_.frameOrPageCount):QString());
    if(still_) {
        setGlyph(play_,imagePlaying_?Glyph::Pause:Glyph::Play);
        if(animationReady() || imageDocument()) {
            time_->setText(imageDocument()?text("Page %1/%2").arg(imageFrameIndex_+1).arg(imageSequence_.frameOrPageCount):QString());
            if(!seek_->isSliderDown()) { QSignalBlocker blocker(seek_); seek_->setValue(imageDocument()?imageFrameIndex_:int(imageDuration()>0?imageTime_*10000/imageDuration():0)); }
        }
        return;
    }
    auto position=player->GetPosition100ns(), total=player->GetDuration100ns();
    if(!seek_->isSliderDown() && !player->IsSeeking()) {
        QSignalBlocker blocker(seek_);
        seek_->setValue(total>0?static_cast<int>(position*10000/total):0);
    }
    if(!seek_->isSliderDown()) {
        time_->setText(duration(position)+" / "+duration(total));
    }
    setGlyph(play_,player->IsPlaying()?Glyph::Pause:Glyph::Play);
    play_->setEnabled(player->IsLoaded()); previous_->setEnabled(player->IsLoaded()); next_->setEnabled(player->IsLoaded());
    setGlyph(mute_,player->IsMuted()?Glyph::Muted:Glyph::Volume);
    mute_->setToolTip(text(player->GetAudioError()==Platform::kAudioErrorNone?"Mute / unmute":"Audio output unavailable"));
    // Repaint only when the player has produced a different frame; a paused video costs nothing per tick.
    if(auto frame=player->GetFrame(); frame!=shownFrame_) { shownFrame_=std::move(frame); canvas->update(); }
}
void PlayerPanel::showImageIndex(int index) {
    if(imageSequence_.playbackFrames.empty()) { canvas->setStillFrame({},true,true); return; }
    index=std::clamp(index,0,int(imageSequence_.frameOrPageCount)-1);
    if(imageDocument() && index!=0) {
        const auto revision=imageRevision_; auto watcher=new QFutureWatcher<QImage>(this);
        imageFrameIndex_=index;
        connect(watcher,&QFutureWatcherBase::finished,this,[this,watcher,revision,index] {
            auto image=watcher->result(); watcher->deleteLater(); if(revision!=imageRevision_ || index!=imageFrameIndex_) return;
            if(image.isNull()) { canvas->setStillFrame({},true,true); return; }
            auto f=std::make_shared<VideoFrame>(); f->width=image.width(); f->height=image.height(); f->pixels.assign(image.constBits(),image.constBits()+image.sizeInBytes()); canvas->setStillFrame(f,true);
        });
        watcher->setFuture(QtConcurrent::run(imagePool_,[source=imageSource_,index] { std::atomic<bool> cancel=false; HRESULT error; return StillImages::readFrameOrPage(source,index,cancel,error); }));
        return;
    }
    imageFrameIndex_=index; const auto& image=imageSequence_.playbackFrames[animationReady()?size_t(index):0];
    auto f=std::make_shared<VideoFrame>(); f->width=image.width(); f->height=image.height(); f->pixels.assign(image.constBits(),image.constBits()+image.sizeInBytes()); canvas->setStillFrame(f,true);
}
void PlayerPanel::seekImage(int64_t time) {
    if(!animationReady()) return;
    imageTimeline_=std::max<int64_t>(0,time); imageTime_=imageTimeline_%imageDuration();
    int64_t end=0; int index=0;
    while(index<int(imageSequence_.frameDurations.size())-1 && (end+=imageSequence_.frameDurations[index])<=imageTime_) ++index;
    if(index!=imageFrameIndex_ || !canvas->frame()) showImageIndex(index);
    imageClock_.restart(); refresh();
}
void PlayerPanel::playImage(bool playing) { imagePlaying_=playing && animationReady() && !imageHidden_; imageClock_.restart(); refresh(); }
void PlayerPanel::stepImage(bool forward) {
    playImage(false); if(!animationReady() && !imageDocument()) return;
    const int index=std::clamp(imageFrameIndex_+(forward?1:-1),0,int(imageSequence_.frameOrPageCount)-1);
    if(imageDocument()) showImageIndex(index);
    else { int64_t time=0; for(int i=0;i<index;++i) time+=imageSequence_.frameDurations[i]; seekImage(time); }
    refresh();
}
void PlayerPanel::tickImage() {
    if(imagePlaying_ && animationReady()) { const auto delta=imageClock_.nsecsElapsed()/100; seekImage(imageTimeline_+delta); }
}

std::shared_ptr<VideoPlayer> PlayerPanel::detachPlayer() {
    imagePlaying_=false; imageSequence_={};
    ++imageRevision_; canvas->setStillFrame({},false);
    auto old=std::move(player); old->RequestStop();
    player=std::make_shared<VideoPlayer>(); player->SetMuted(old->IsMuted());
    canvas->setPlayer(player.get()); path_.clear(); fileIndex=-1;
    return old;
}
