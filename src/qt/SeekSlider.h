#pragma once
#include <QSlider>
#include <QMouseEvent>
#include <QStyleOptionSlider>
#include <algorithm>
#include <cmath>

class SeekSlider final : public QSlider {
public:
    explicit SeekSlider(QWidget* parent=nullptr):QSlider(Qt::Horizontal,parent) { setMinimumHeight(28); }
protected:
    void mousePressEvent(QMouseEvent* event) override {
        if(event->button()!=Qt::LeftButton) { QSlider::mousePressEvent(event); return; }
        setFocus(Qt::MouseFocusReason);
        QStyleOptionSlider option; initStyleOption(&option);
        const auto handle=style()->subControlRect(QStyle::CC_Slider,&option,QStyle::SC_SliderHandle,this);
        // Keep a nearby grab from moving the thumb; the whole rail also supports direct scrubbing.
        grabOffset_=std::abs(event->position().x()-handle.center().x())<=14?event->position().x()-handle.center().x():0;
        setSliderDown(true); moveTo(event->position().x()); event->accept();
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        if(!isSliderDown()) { QSlider::mouseMoveEvent(event); return; }
        moveTo(event->position().x()); event->accept();
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        if(event->button()==Qt::LeftButton && isSliderDown()) {
            moveTo(event->position().x()); setSliderDown(false); event->accept();
        } else QSlider::mouseReleaseEvent(event);
    }
private:
    double grabOffset_=0;
    void moveTo(double x) {
        QStyleOptionSlider option; initStyleOption(&option);
        const auto groove=style()->subControlRect(QStyle::CC_Slider,&option,QStyle::SC_SliderGroove,this);
        const auto handle=style()->subControlRect(QStyle::CC_Slider,&option,QStyle::SC_SliderHandle,this);
        const int span=groove.width()-handle.width();
        setSliderPosition(QStyle::sliderValueFromPosition(minimum(),maximum(),qRound(x-grabOffset_-groove.x()-handle.width()/2.0),std::max(1,span),option.upsideDown));
    }
};
