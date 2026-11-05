#include "ResultsDelegate.h"
#include "MainWindow.h"
#include <QAbstractItemView>
#include <QApplication>
#include <QHelpEvent>
#include <QPainter>
#include <QStyle>
#include <QToolTip>

ResultsDelegate::ResultsDelegate(MainWindow* window) : QStyledItemDelegate(window), window_(window) {}
void ResultsDelegate::paint(QPainter* painter,const QStyleOptionViewItem& option,const QModelIndex& index) const {
    QStyleOptionViewItem item(option); initStyleOption(&item,index); item.text.clear(); item.icon={};
    const auto style=item.widget?item.widget->style():QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem,&item,painter,item.widget); // row background, selection and divider
    window_->paintGroupRow(painter,item,index.data(Qt::UserRole).toInt());
}
QSize ResultsDelegate::sizeHint(const QStyleOptionViewItem&,const QModelIndex&) const { return {0,ResultRow::height}; }
bool ResultsDelegate::helpEvent(QHelpEvent* event,QAbstractItemView* view,const QStyleOptionViewItem& option,const QModelIndex& index) {
    const int group=index.data(Qt::UserRole).toInt(); const int slot=window_->thumbnailAt(group,event->pos());
    if(event->type()==QEvent::ToolTip && slot>=0) { QToolTip::showText(event->globalPos(),window_->thumbnailToolTip(group,slot),view->viewport()); return true; }
    return QStyledItemDelegate::helpEvent(event,view,option,index);
}
