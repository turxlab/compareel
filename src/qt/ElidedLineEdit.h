#pragma once
#include <QFocusEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QResizeEvent>
#include <QStyle>
#include <QStyleOptionFrame>

// A read-only field that shows "start…end" when the text does not fit, so the distinguishing end of a
// file name or folder stays visible. Focusing it restores the full text for selection and copying.
class ElidedLineEdit final : public QLineEdit {
public:
    explicit ElidedLineEdit(QWidget* parent=nullptr) : QLineEdit(parent) { setReadOnly(true); }
    void setFullText(const QString& value) { full_=value; refresh(); }
    const QString& fullText() const { return full_; }
protected:
    void resizeEvent(QResizeEvent* event) override { QLineEdit::resizeEvent(event); refresh(); }
    void focusInEvent(QFocusEvent* event) override { QLineEdit::setText(full_); setCursorPosition(0); QLineEdit::focusInEvent(event); }
    void focusOutEvent(QFocusEvent* event) override { QLineEdit::focusOutEvent(event); refresh(); }
private:
    QString full_;
    void refresh() {
        if(hasFocus()) { if(text()!=full_) { QLineEdit::setText(full_); setCursorPosition(0); } return; }
        QStyleOptionFrame option; initStyleOption(&option);
        const int width=style()->subElementRect(QStyle::SE_LineEditContents,&option,this).width()-textMargins().left()-textMargins().right()-4;
        QLineEdit::setText(fontMetrics().horizontalAdvance(full_)<=width?full_:fontMetrics().elidedText(full_,Qt::ElideMiddle,std::max(width,0)));
        setCursorPosition(0);
    }
};

// A one-line label that ends in "…" when squeezed instead of being cut off mid-word; text() keeps the full value.
class ElidedLabel final : public QLabel {
public:
    using QLabel::QLabel;
    QSize minimumSizeHint() const override { return {0,QLabel::minimumSizeHint().height()}; }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this); painter.setPen(palette().color(foregroundRole()));
        painter.drawText(contentsRect(),int(alignment()|Qt::TextSingleLine),fontMetrics().elidedText(text(),Qt::ElideRight,contentsRect().width()));
    }
};
