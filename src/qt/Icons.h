#pragma once
#include <QFile>
#include <QHash>
#include <QIcon>
#include <QIconEngine>
#include <QPainter>
#include <QPixmapCache>
#include <QSvgRenderer>
#include "Theme.h"

enum class Glyph { Play, Pause, Previous, Next, PreviousGroup, NextGroup, Restart, RestartVideo,
    Folder, Trash, Volume, Muted, Lock, Unlock, Stop, Settings, Eye, EyeOff, Link, Unlink, Align, Files, Search, Check, Clock };

// Keep the original vector geometry at every requested icon size. A themed icon looks its colour up
// when painted, so icons already on screen follow a theme switch without being set again.
class LucideIconEngine final : public QIconEngine {
public:
    LucideIconEngine(QByteArray svg,QByteArray token,QColor color) : svg_(std::move(svg)), token_(std::move(token)), color_(color) {}
    QIconEngine* clone() const override { return new LucideIconEngine(svg_,token_,color_); }
    // Rendered pixmaps are cached per glyph, colour and device size, so repaints do not parse the SVG again.
    void paint(QPainter* painter,const QRect& rect,QIcon::Mode mode,QIcon::State) override {
        const QColor color=token_.isEmpty()?color_:Theme::color(token_.constData());
        const int side=std::min(rect.width(),rect.height()); if(side<=0) return;
        const qreal ratio=painter->device()?painter->device()->devicePixelRatioF():1.0; const int pixels=std::max(1,int(std::ceil(side*ratio)));
        const auto key=QString("lucide|%1|%2|%3").arg(qHash(svg_)).arg(color.rgba()).arg(pixels);
        QPixmap rendered;
        if(!QPixmapCache::find(key,&rendered)) {
            auto svg=svg_; svg.replace("currentColor",color.name().toUtf8());
            rendered=QPixmap(pixels,pixels); rendered.fill(Qt::transparent);
            { QPainter target(&rendered); target.setRenderHint(QPainter::Antialiasing); QSvgRenderer(svg).render(&target,QRectF(0,0,pixels,pixels)); }
            QPixmapCache::insert(key,rendered);
        }
        rendered.setDevicePixelRatio(ratio);
        painter->save(); if(mode==QIcon::Disabled) painter->setOpacity(painter->opacity()*0.35);
        painter->drawPixmap(QRect(rect.x()+(rect.width()-side)/2,rect.y()+(rect.height()-side)/2,side,side),rendered); painter->restore();
    }
    QPixmap pixmap(const QSize& size,QIcon::Mode mode,QIcon::State state) override {
        QPixmap result(size); result.fill(Qt::transparent); QPainter painter(&result);
        paint(&painter,QRect(QPoint(0,0),size),mode,state); return result;
    }
private:
    QByteArray svg_,token_;
    QColor color_;
};

inline QIcon glyphIcon(Glyph glyph,const QByteArray& token,QColor color) {
    const char* name="settings-2";
    switch(glyph) {
    case Glyph::Play: name="play"; break;
    case Glyph::Pause: name="pause"; break;
    case Glyph::Previous: name="step-back"; break;
    case Glyph::Next: name="step-forward"; break;
    case Glyph::PreviousGroup: name="skip-back"; break;
    case Glyph::NextGroup: name="skip-forward"; break;
    case Glyph::Restart: name="refresh-cw"; break;
    case Glyph::RestartVideo: name="rotate-ccw"; break;
    case Glyph::Folder: name="folder-open"; break;
    case Glyph::Trash: name="trash"; break;
    case Glyph::Volume: name="volume-2"; break;
    case Glyph::Muted: name="volume-x"; break;
    case Glyph::Lock: name="lock"; break;
    case Glyph::Unlock: name="lock-open"; break;
    case Glyph::Stop: name="square"; break;
    case Glyph::Eye: name="eye"; break;
    case Glyph::EyeOff: name="eye-off"; break;
    case Glyph::Link: name="link"; break;
    case Glyph::Unlink: name="unlink"; break;
    case Glyph::Align: name="move-horizontal"; break;
    case Glyph::Files: name="files"; break;
    case Glyph::Search: name="search"; break;
    case Glyph::Check: name="check"; break;
    case Glyph::Clock: name="clock"; break;
    case Glyph::Settings: break;
    }
    static QHash<QString,QIcon> cache;
    const auto key=QString("%1|%2|%3").arg(int(glyph)).arg(QString::fromUtf8(token)).arg(color.rgba());
    if(auto found=cache.constFind(key); found!=cache.cend()) return *found;
    QFile file(QString(":/lucide/%1.svg").arg(name));
    if(!file.open(QIODevice::ReadOnly)) return {};
    auto result=QIcon(new LucideIconEngine(file.readAll(),token,color)); cache.insert(key,result); return result;
}
// Setting an icon repaints its button; the 30 Hz tick uses this to skip it when the glyph is unchanged.
template<class T> void setGlyph(T* target,Glyph glyph) {
    if(target->property("glyph").toInt()==int(glyph)+1) return;
    target->setProperty("glyph",int(glyph)+1); target->setIcon(glyphIcon(glyph,"@icon",{}));
}
// Themed by a design token ("@icon" unless stated), or a fixed colour for marks drawn on media.
inline QIcon icon(Glyph glyph,const char* token="@icon") { return glyphIcon(glyph,token,{}); }
inline QIcon icon(Glyph glyph,QColor color) { return glyphIcon(glyph,{},color); }
