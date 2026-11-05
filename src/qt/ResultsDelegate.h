#pragma once
#include <QStyledItemDelegate>

class MainWindow;

// Geometry of a painted result row (pixels): summary line, then a strip of thumbnails with captions.
namespace ResultRow {
inline constexpr int height=120, margin=4, top=3, metaHeight=16, cellTop=top+metaHeight+3, cell=110, spacing=4, thumbHeight=62, nameHeight=16, scroller=22;
}

// Paints result rows from the window's groups, so the list holds plain items instead of a widget tree per row.
class ResultsDelegate final : public QStyledItemDelegate {
public:
    explicit ResultsDelegate(MainWindow* window);
    void paint(QPainter* painter,const QStyleOptionViewItem& option,const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem&,const QModelIndex&) const override;
    bool helpEvent(QHelpEvent* event,QAbstractItemView* view,const QStyleOptionViewItem& option,const QModelIndex& index) override;
private:
    MainWindow* window_;
};
