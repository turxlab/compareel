#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "ResultsDelegate.h"
#include "Language.h"
#include "Icons.h"
#include "Theme.h"
#include "FileOperations.h"
#include "platform/Platform.h"
#include "ScanCache.h"
#include "ImageReader.h"
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFutureWatcher>
#include <QHelpEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPixmapCache>
#include <QPointer>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSplitter>
#include <QToolButton>
#include <QToolTip>
#include <QStatusBar>
#include <QtConcurrent/QtConcurrentRun>

using Language::text;

namespace {
// Marked files are dimmed so the strip reads "leaving" without hovering each thumbnail.
QIcon thumbnailIcon(const VideoFileInfo& file,bool marked) {
    QImage image(file.thumbBGRA.data(),THUMB_W,THUMB_H,THUMB_W*4,QImage::Format_ARGB32_Premultiplied);
    QPixmap pixmap=QPixmap::fromImage(image);
    if(marked) { QPainter painter(&pixmap); painter.setCompositionMode(QPainter::CompositionMode_SourceAtop); painter.fillRect(pixmap.rect(),QColor(11,14,19,140)); }
    return QIcon(pixmap);
}
}

void MainWindow::updateGroupRow(int groupIndex) {
    if(groupIndex<0 || groupIndex>=static_cast<int>(groups_.size()) || groupIndex>=results_->topLevelItemCount()) return;
    auto& group=groups_[groupIndex];
    if(group.fileIndices.size()<2) return;
    group.leftSlot=0;
    group.rightSlot=std::clamp(group.rightSlot,1,static_cast<int>(group.fileIndices.size())-1);
    // A re-sort may move a marked file to slot 0; the mark stays because removal always keeps an unmarked match.
    for(int member:group.fileIndices) if(files_[member].protectedFile) markedFiles_.remove(member);
    auto item=results_->topLevelItem(groupIndex);
    QSignalBlocker blocker(results_);
    const bool selectionChanged=item->data(0,Qt::UserRole+1).toInt()!=group.rightSlot;
    item->setData(0,Qt::UserRole,groupIndex);
    item->setData(0,Qt::UserRole+1,group.rightSlot);
    QStringList allPaths; for(int member:group.fileIndices) allPaths<<QString::fromStdWString(files_[member].fullPath);
    item->setToolTip(0,allPaths.join('\n'));
    item->setSizeHint(0,QSize(0,ResultRow::height));
    // The row is painted, so a screen reader gets one sentence for it: summary, reference and compared candidate.
    item->setData(0,Qt::AccessibleTextRole,text("Group %1").arg(groupIndex+1)+"  ·  "+groupDetails(groupIndex).join("  ·  ")+"  ·  "+
        text("Reference · shown on the left")+": "+QString::fromStdWString(files_[group.fileIndices[0]].fileName)+"  ·  "+
        text("Compared candidate · shown on the right")+": "+QString::fromStdWString(files_[group.fileIndices[group.rightSlot]].fileName));
    if(selectionChanged && currentGroup_==groupIndex) ensureThumbnailVisible(groupIndex,group.rightSlot);
    results_->viewport()->update(results_->visualItemRect(item));
}

// Row geometry shared by painting, hit testing and the self-checks.
QTreeWidgetItem* MainWindow::groupItem(int group) const {
    return group>=0 && group<results_->topLevelItemCount()?results_->topLevelItem(group):nullptr;
}
QRect MainWindow::thumbnailRect(int group,int slot) const {
    auto item=groupItem(group); if(!item || slot<0 || slot>=int(groups_[group].fileIndices.size())) return {};
    const auto row=results_->visualItemRect(item);
    return {row.left()+ResultRow::margin+slot*(ResultRow::cell+ResultRow::spacing)-rowScroll(group),row.top()+ResultRow::cellTop,ResultRow::cell,ResultRow::thumbHeight};
}
int MainWindow::rowScroll(int group) const { auto item=groupItem(group); return item?std::clamp(item->data(0,Qt::UserRole+2).toInt(),0,rowScrollMax(group)):0; }
int MainWindow::rowScrollMax(int group) const {
    if(group<0 || group>=int(groups_.size())) return 0;
    const int count=int(groups_[group].fileIndices.size());
    const int content=2*ResultRow::margin+count*ResultRow::cell+std::max(0,count-1)*ResultRow::spacing;
    return std::max(0,content-results_->viewport()->width());
}
void MainWindow::setRowScroll(int group,int pixels) {
    auto item=groupItem(group); if(!item) return;
    pixels=std::clamp(pixels,0,rowScrollMax(group));
    if(item->data(0,Qt::UserRole+2).toInt()==pixels) return;
    { QSignalBlocker blocker(results_); item->setData(0,Qt::UserRole+2,pixels); }
    results_->viewport()->update(results_->visualItemRect(item));
    QTimer::singleShot(0,this,[this] { requestVisiblePreviews(); });
}
void MainWindow::ensureThumbnailVisible(int group,int slot) {
    if(!groupItem(group) || slot<0) return;
    const int left=ResultRow::margin+slot*(ResultRow::cell+ResultRow::spacing),width=results_->viewport()->width();
    // Scrollers cover the row's edges, so a visible thumbnail must clear them too.
    int scroll=rowScroll(group);
    if(left-ResultRow::scroller<scroll) scroll=left-ResultRow::scroller;
    else if(left+ResultRow::cell+ResultRow::scroller>scroll+width) scroll=left+ResultRow::cell+ResultRow::scroller-width;
    setRowScroll(group,scroll);
}
int MainWindow::thumbnailAt(int group,QPoint position) const {
    auto item=groupItem(group); if(!item) return -1;
    const auto row=results_->visualItemRect(item);
    if(position.y()>=row.top()+ResultRow::cellTop && position.y()<row.top()+ResultRow::cellTop+ResultRow::thumbHeight) {
        if(rowScroll(group)>0 && position.x()<row.left()+ResultRow::scroller) return -2;
        if(rowScroll(group)<rowScrollMax(group) && position.x()>=row.right()-ResultRow::scroller) return -3;
    }
    for(int slot=0;slot<int(groups_[group].fileIndices.size());++slot) {
        const auto cell=thumbnailRect(group,slot);
        if(QRect(cell.left(),cell.top(),cell.width(),cell.height()+ResultRow::nameHeight).contains(position)) return slot;
    }
    return -1;
}
QStringList MainWindow::groupDetails(int groupIndex,int* proofOut) const {
    const auto& group=groups_[groupIndex]; const int leftIdx=group.fileIndices[0];
    uint64_t bytes=0; int decided=0,marked=0;
    for(int slot=0;slot<int(group.fileIndices.size());++slot) {
        const int member=group.fileIndices[slot]; bytes+=files_[member].fileSize; if(markedFiles_.contains(member)) ++marked;
        if(slot>0) if(auto found=reviewDecisions_.find(ReviewPairKey(files_[leftIdx].identity,files_[member].identity)); found!=reviewDecisions_.end() && found->second!=ReviewDecision::Unreviewed) ++decided;
    }
    const int proof=proofLevel(group); if(proofOut) *proofOut=proof;
    QStringList parts{text("%1 files").arg(group.fileIndices.size()),QString::fromStdWString(FormatBytesShort(bytes)),
        text(proof==2?"Exact copies":proof==1?"Includes exact copies":"Similar matches"),text("%1/%2 pairs reviewed").arg(decided).arg(group.fileIndices.size()-1)};
    if(marked) parts<<text("%1 marked for cleanup").arg(marked);
    return parts;
}
MainWindow::ThumbnailView MainWindow::thumbnailView(int groupIndex,int slot) const {
    ThumbnailView view; if(groupIndex<0 || groupIndex>=int(groups_.size()) || slot<0 || slot>=int(groups_[groupIndex].fileIndices.size())) return view;
    const auto& group=groups_[groupIndex]; const int index=group.fileIndices[slot]; const auto& file=files_[index];
    view.fileIndex=index; view.selected=slot==group.rightSlot; view.previewHidden=hideThumbnailsAction_->isChecked();
    view.hasImage=!view.previewHidden && file.thumbBGRA.size()==THUMB_W*THUMB_H*4;
    view.caption=results_->fontMetrics().elidedText(QString::fromStdWString(file.fileName),Qt::ElideMiddle,ResultRow::cell-4);
    // The top-right badge shows a mark, or else protection; the bottom-right one the pair decision.
    if(markedFiles_.contains(index)) view.badges<<text("Marked for cleanup"); else if(file.protectedFile) view.badges<<text("Protected");
    if(slot>0) if(auto found=reviewDecisions_.find(ReviewPairKey(files_[group.fileIndices[0]].identity,file.identity)); found!=reviewDecisions_.end() && found->second!=ReviewDecision::Unreviewed)
        view.badges<<text(reviewLabels[int(found->second)]);
    return view;
}
QString MainWindow::thumbnailToolTip(int groupIndex,int slot) const {
    const auto& group=groups_[groupIndex]; const int index=group.fileIndices[slot]; const auto& file=files_[index];
    QStringList states;
    if(slot==0) states<<text("Reference · shown on the left"); else if(slot==group.rightSlot) states<<text("Compared candidate · shown on the right");
    if(markedFiles_.contains(index)) states<<text("Marked for cleanup");
    if(file.protectedFile) states<<text("Protected");
    if(slot>0) if(auto found=reviewDecisions_.find(ReviewPairKey(files_[group.fileIndices[0]].identity,file.identity)); found!=reviewDecisions_.end() && found->second!=ReviewDecision::Unreviewed)
        states<<text(reviewLabels[int(found->second)]);
    return thumbnailTip(file)+(states.isEmpty()?QString():"\n"+states.join('\n'));
}
void MainWindow::paintGroupRow(QPainter* painter,const QStyleOptionViewItem& option,int groupIndex) const {
    if(groupIndex<0 || groupIndex>=int(groups_.size()) || groups_[groupIndex].fileIndices.size()<2) return;
    const auto& group=groups_[groupIndex]; const auto row=option.rect; const int scroll=rowScroll(groupIndex);
    painter->save(); painter->setClipRect(row); painter->setRenderHint(QPainter::Antialiasing);
    // Summary line: what kind of evidence, how much space, and how far the review got.
    int proof=0; const auto details=groupDetails(groupIndex,&proof);
    QFont summaryFont=option.font; summaryFont.setPixelSize(11); painter->setFont(summaryFont);
    painter->setPen(Theme::color(proof==2?"@proofText":"@muted"));
    const QRect metaRect(row.left()+ResultRow::margin,row.top()+ResultRow::top,row.width()-2*ResultRow::margin,ResultRow::metaHeight);
    // Absolute: Arabic or Persian text would otherwise right-align above left-aligned thumbnails.
    painter->drawText(metaRect,Qt::AlignLeft|Qt::AlignAbsolute|Qt::AlignVCenter,QFontMetrics(summaryFont).elidedText(text("Group %1").arg(groupIndex+1)+"  ·  "+details.join("  ·  "),Qt::ElideRight,metaRect.width()));
    const bool hidden=hideThumbnailsAction_->isChecked(); const bool listFocused=results_->hasFocus();
    const QColor dark("#0b0e13");
    auto badge=[&](QRect rect,const QColor& background) { painter->setPen(Qt::NoPen); painter->setBrush(background); painter->drawRoundedRect(rect,3,3); };
    for(int slot=0;slot<int(group.fileIndices.size());++slot) {
        const QRect cell(row.left()+ResultRow::margin+slot*(ResultRow::cell+ResultRow::spacing)-scroll,row.top()+ResultRow::cellTop,ResultRow::cell,ResultRow::thumbHeight);
        if(cell.right()<row.left() || cell.left()>row.right()) continue;
        const int index=group.fileIndices[slot]; const auto& file=files_[index]; const bool marked=markedFiles_.contains(index);
        // Border: role first (compared, marked, reference), then hover, then keyboard focus on the compared thumbnail.
        const char* border=slot==group.rightSlot?"@thumbSelected":marked?"@thumbMarked":slot==0?"@thumbReference":"@thumbBorder";
        QColor stroke=Theme::color(border);
        if(groupIndex==hoveredGroup_ && slot==hoveredSlot_) stroke=Theme::color("@thumbHover");
        if(listFocused && groupIndex==currentGroup_ && slot==group.rightSlot) stroke=Theme::color("@thumbFocus");
        painter->setPen(QPen(stroke,2)); painter->setBrush(Theme::color("@mediaFrame"));
        painter->drawRoundedRect(QRectF(cell).adjusted(1,1,-1,-1),3,3);
        const QRect inner=cell.adjusted(4,4,-4,-4);
        if(hidden) icon(Glyph::EyeOff,QColor("#7e91a4")).paint(painter,QRect(inner.center()-QPoint(18,18),QSize(36,36)));
        else if(file.thumbBGRA.size()==THUMB_W*THUMB_H*4) {
            // Pixmaps are cached per file buffer; a refreshed preview has a new buffer and so a new key.
            const auto key=QString("resultThumb|%1|%2|%3").arg(index).arg(quintptr(file.thumbBGRA.data())).arg(marked);
            QPixmap pixmap; if(!QPixmapCache::find(key,&pixmap)) { pixmap=thumbnailIcon(file,marked).pixmap(QSize(THUMB_W,THUMB_H)); QPixmapCache::insert(key,pixmap); }
            painter->drawPixmap(QRect(inner.center().x()-THUMB_W/2+1,inner.center().y()-THUMB_H/2+1,THUMB_W,THUMB_H),pixmap);
        } else { painter->setPen(Theme::color("@mediaText")); painter->drawText(inner,Qt::AlignCenter,QString::fromUtf8("—")); }
        // Role badge: the reference carries a word, the compared candidate an eye; colour alone does not tell them apart.
        if(slot==0 || slot==group.rightSlot) {
            QFont roleFont=summaryFont; roleFont.setPixelSize(10); roleFont.setBold(true);
            const int width=slot==0?QFontMetrics(roleFont).horizontalAdvance(text("Ref"))+6:16;
            const QRect rect(cell.left()+4,cell.top()+4,std::max(16,width),16);
            badge(rect,QColor(slot==0?"#6fb1aa":"#70c5e0"));
            if(slot==0) { painter->setFont(roleFont); painter->setPen(dark); painter->drawText(rect,Qt::AlignCenter,text("Ref")); painter->setFont(summaryFont); }
            else icon(Glyph::Eye,dark).paint(painter,rect.adjusted(2,2,-3,-3));
        }
        auto state=[&](Glyph glyph,const char* color,QPoint at) { const QRect rect(at,QSize(16,16)); badge(rect,QColor(color)); icon(glyph,dark).paint(painter,rect.adjusted(2,2,-3,-3)); };
        const QPoint topRight(cell.right()-19,cell.top()+4),bottomRight(cell.right()-19,cell.bottom()-19);
        if(marked) state(Glyph::Trash,"#e07a7a",topRight); else if(file.protectedFile) state(Glyph::Lock,"#d8c27a",topRight);
        if(slot>0) if(auto found=reviewDecisions_.find(ReviewPairKey(files_[group.fileIndices[0]].identity,file.identity)); found!=reviewDecisions_.end()) {
            if(found->second==ReviewDecision::Reviewed) state(Glyph::Check,"#8fd3a8",bottomRight);
            else if(found->second==ReviewDecision::KeepBoth) state(Glyph::Files,"#8ab8e6",bottomRight);
            else if(found->second==ReviewDecision::Later) state(Glyph::Clock,"#e6c26e",bottomRight);
        }
        painter->setFont(option.font); painter->setPen(option.palette.color(QPalette::Text));
        const QRect name(cell.left(),cell.bottom()+1,ResultRow::cell,ResultRow::nameHeight);
        painter->drawText(name,Qt::AlignHCenter|Qt::AlignVCenter,option.fontMetrics.elidedText(QString::fromStdWString(file.fileName),Qt::ElideMiddle,ResultRow::cell-4));
        painter->setFont(summaryFont);
    }
    // Scrollers: a row with more thumbnails than fit scrolls sideways with these, Shift+wheel or the arrow keys.
    auto scroller=[&](bool leftSide) {
        const QRect rect(leftSide?row.left():row.right()-ResultRow::scroller+1,row.top()+ResultRow::cellTop,ResultRow::scroller,ResultRow::thumbHeight);
        QColor fill=Theme::color("@raised"); fill.setAlpha(230); painter->setPen(Qt::NoPen); painter->setBrush(fill); painter->drawRect(rect);
        const QPointF c=QRectF(rect).center(); const qreal d=leftSide?-4:4;
        QPainterPath arrow; arrow.moveTo(c.x()-d,c.y()-7); arrow.lineTo(c.x()+d,c.y()); arrow.lineTo(c.x()-d,c.y()+7);
        painter->setBrush(Qt::NoBrush); painter->setPen(QPen(Theme::color("@icon"),2,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin)); painter->drawPath(arrow);
    };
    if(scroll>0) scroller(true);
    if(scroll<rowScrollMax(groupIndex)) scroller(false);
    painter->restore();
}
void MainWindow::showThumbnailMenu(int groupIndex,int slot,QPoint global) {
    if(scanning_ || fileOperationPending_ || groupIndex<0 || groupIndex>=int(groups_.size()) || slot<0 || slot>=int(groups_[groupIndex].fileIndices.size())) return;
    selectGroup(groupIndex,slot==0?-1:slot);
    QMenu menu(this); menu.setObjectName("thumbnailMenu");
    if(slot>0) { menu.addAction(text("Make reference"),this,[this,groupIndex,slot] { makeReference(groupIndex,slot); }); menu.addSeparator(); }
    addTransferActions(&menu,groups_[groupIndex].fileIndices[slot]); menu.addSeparator();
    for(auto item:reviewActions_) menu.addAction(item);
    menu.exec(global);
}
bool MainWindow::clickThumbnail(int groupIndex,int slot,Qt::MouseButton button) {
    auto item=groupItem(groupIndex); if(!item) return false;
    results_->scrollToItem(item); ensureThumbnailVisible(groupIndex,slot); QApplication::processEvents();
    const auto rect=thumbnailRect(groupIndex,slot); if(rect.isEmpty()) return false;
    const QPoint point=rect.center(); const auto global=results_->viewport()->mapToGlobal(point);
    if(button==Qt::RightButton) { QContextMenuEvent event(QContextMenuEvent::Mouse,point,global); QApplication::sendEvent(results_->viewport(),&event); return true; }
    QMouseEvent press(QEvent::MouseButtonPress,QPointF(point),QPointF(global),button,button,Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease,QPointF(point),QPointF(global),button,Qt::NoButton,Qt::NoModifier);
    QApplication::sendEvent(results_->viewport(),&press); QApplication::sendEvent(results_->viewport(),&release);
    return true;
}

void MainWindow::populateResults() {
    reviewDirty_=true;
    int scroll=results_->verticalScrollBar()->value();
    QSignalBlocker blocker(results_); results_->clear();
    for(int i=0;i<static_cast<int>(groups_.size());++i) {
        if(groups_[i].fileIndices.size()<2) continue;
        auto item=new QTreeWidgetItem(results_);
        updateGroupRow(i);
        if(i==currentGroup_) results_->setCurrentItem(item);
    }
    finishResultsUpdate(scroll);
}
void MainWindow::finishResultsUpdate(int scroll) {
    QSet<int> uniqueFiles;
    for(const auto& group:groups_) for(int member:group.fileIndices) uniqueFiles.insert(member);
    resultSummary_->setText(groups_.size()==1?text("1 group · %1 files").arg(uniqueFiles.size()):text("%1 groups · %2 files").arg(groups_.size()).arg(uniqueFiles.size()));
    updateSpaceSummary();
    applyFilter(); results_->verticalScrollBar()->setValue(scroll);
    updateEmptyState();
    QTimer::singleShot(0,this,[this] { requestVisiblePreviews(); });
}
// Replaces the row of one group with rows for the groups that took its place, leaving every other row widget intact.
void MainWindow::replaceGroupRows(int groupIndex,int insertedGroups) {
    reviewDirty_=true;
    const int scroll=results_->verticalScrollBar()->value();
    QSignalBlocker blocker(results_);
    delete results_->takeTopLevelItem(groupIndex);
    for(int i=0;i<insertedGroups;++i) { results_->insertTopLevelItem(groupIndex+i,new QTreeWidgetItem); updateGroupRow(groupIndex+i); }
    renumberGroupRows(groupIndex+insertedGroups);
    finishResultsUpdate(scroll);
}
void MainWindow::renumberGroupRows(int first) {
    for(int i=std::max(0,first);i<results_->topLevelItemCount();++i) {
        auto item=results_->topLevelItem(i); if(item->data(0,Qt::UserRole).toInt()==i) continue;
        QSignalBlocker blocker(results_); item->setData(0,Qt::UserRole,i);
    }
    results_->viewport()->update();
}

void MainWindow::updateSpaceSummary() {
    auto key=[](const VideoFileInfo& file) {
        return file.identity.valid ? QString::number(file.identity.volume)+":"+QString::number(file.identity.index)
                                   : pathKey(QString::fromStdWString(file.fullPath));
    };
    QSet<QString> keepers,seen;
    bool exact=!groups_.empty();
    for(const auto& group:groups_) {
        exact=exact && proofLevel(group)==2;
        for(int index:group.fileIndices) if(!files_[index].deleted) { keepers.insert(key(files_[index])); break; }
    }
    uint64_t total=0,other=0;
    for(const auto& group:groups_) for(int index:group.fileIndices) {
        const auto& file=files_[index]; if(file.deleted) continue;
        const auto id=key(file); if(seen.contains(id)) continue; seen.insert(id);
        total+=file.fileSize;
        if(!keepers.contains(id)) other+=file.fileSize;
    }
    spaceSummary_->setText(text(exact?"Total: %1 · Potential savings: %2":"Total: %1 · Extra copies: %2")
        .arg(QString::fromStdWString(FormatBytesShort(total)),QString::fromStdWString(FormatBytesShort(other))));
    spaceSummary_->setToolTip(text("All result groups, including filtered-out ones; each file is counted once. The second figure leaves out the first file of each group. It is an estimate, not a cleanup plan."));
    spaceSummary_->setVisible(!seen.empty());
}

void MainWindow::clearResults() {
    ++generation_; syncing_=false; pendingPreviews_.clear(); previewRuns_.clear(); requestedSamples_.clear(); markedFiles_.clear();
    left_->setFile(nullptr,-1); right_->setFile(nullptr,-1); right_->setCandidates(nullptr);
    files_.clear(); groups_.clear(); scanIssues_.clear(); manual_=false; currentGroup_=-1; partialVerdict_.clear(); scanCompletedThisSession_=false;
    populateResults(); updateSimilarity(); updateEmptyState();
}

void MainWindow::applyFilter() {
    const auto value=filter_->text().trimmed();
    for(int i=0;i<results_->topLevelItemCount();++i) {
        auto item=results_->topLevelItem(i);
        bool match=value.isEmpty() || item->toolTip(0).contains(value,Qt::CaseInsensitive);
        const int proof=i<int(groups_.size())?proofLevel(groups_[i]):0;
        const int choice=evidenceFilter_?evidenceFilter_->currentIndex():0;
        const bool evidenceMatch=choice==0 || (choice==1 && proof==2) || (choice==2 && proof>=1) || (choice==3 && proof!=2);
        item->setHidden(!match || !evidenceMatch);
    }
    // An empty list must say why: a filter hides every group. Similar scans have no content hashes yet, so the
    // exact-copy filters stay empty until the results are verified.
    int visible=0; for(int i=0;i<results_->topLevelItemCount();++i) visible+=!results_->topLevelItem(i)->isHidden();
    const int choice=evidenceFilter_?evidenceFilter_->currentIndex():0;
    if(visible==0 && results_->topLevelItemCount()>0 && (!value.isEmpty() || choice!=0)) {
        const bool unverified=(choice==1 || choice==2) && std::none_of(groups_.begin(),groups_.end(),[this](const auto& group) { return proofLevel(group)>0; });
        statusBar()->showMessage(unverified?text("No groups match this filter. Similar scans do not compare file contents; use Tools > Verify exact matches in results first."):text("No groups match this filter."));
    }
    QTimer::singleShot(0,this,[this] { requestVisiblePreviews(); });
}

void MainWindow::requestVisiblePreviews() {
    if(scanning_ || sortingMetadata_ || closing_ || fileOperationPending_ || hideThumbnailsAction_->isChecked()) return;
    const auto viewport=results_->viewport()->rect();
    auto item=results_->itemAt(QPoint(1,0));
    if(!item) item=results_->itemAt(QPoint(1,4));
    while(item && results_->visualItemRect(item).intersects(viewport)) {
        const int group=item->data(0,Qt::UserRole).toInt();
        if(group>=0 && group<static_cast<int>(groups_.size()))
            for(int slot=0;slot<int(groups_[group].fileIndices.size());++slot) if(thumbnailRect(group,slot).intersects(viewport)) requestPreview(groups_[group].fileIndices[slot]);
        item=results_->itemBelow(item);
    }
}

void MainWindow::selectGroup(int group,int comparison) {
    if(scanning_ || fileOperationPending_ || group<0 || group>=static_cast<int>(groups_.size())) return;
    reviewDirty_=true;
    bool keepFocus=results_->hasFocus() || results_->isAncestorOf(QApplication::focusWidget());
    int previousGroup=currentGroup_;
    restorePositions_.reset(); currentGroup_=group; auto& selected=groups_[group];
    int targetRight=comparison>=1?comparison:selected.rightSlot;
    selected.leftSlot=0; selected.rightSlot=std::clamp(targetRight,1,static_cast<int>(selected.fileIndices.size())-1);
    syncing_=false; int left=selected.fileIndices[0], right=selected.fileIndices[selected.rightSlot];
    if(left_->fileIndex!=left || right_->fileIndex!=right) { left_->playImage(false); right_->playImage(false); left_->seekImage(0); right_->seekImage(0); linkedStepFromRight_.reset(); lastControlledRight_=false; linkAction_->setChecked(true); linkAction_->setIcon(icon(Glyph::Link)); QSignalBlocker blocker(offset_); offset_->setValue(0); alignmentOffset100ns_=0; }
    left_->setFile(&files_[left],left); right_->setFile(&files_[right],right);
    left_->setCandidates(nullptr); right_->setCandidates(&selected);
    showGroupPosition();
    previousAction_->setEnabled(group>0); nextAction_->setEnabled(group+1<static_cast<int>(groups_.size()));
    auto item=results_->topLevelItem(group);
    if(item && item->data(0,Qt::UserRole+1).toInt()!=selected.rightSlot) {
        updateGroupRow(group);
        item=results_->topLevelItem(group);
    }
    if(item) {
        QSignalBlocker blocker(results_);
        results_->setCurrentItem(item);
        results_->scrollToItem(item);
    }
    if(previousGroup>=0 && previousGroup!=group) updateGroupRow(previousGroup);
    updateSimilarity();
    updateReviewState();
    requestPreview(left); requestPreview(right);
    ensureThumbnailVisible(group,selected.rightSlot);
    if(keepFocus) results_->setFocus(Qt::OtherFocusReason);
    results_->viewport()->update();
}

void MainWindow::requestPreview(int index) {
    if(index<0 || index>=static_cast<int>(files_.size())) return;
    bool sampleVideo=requestedSamples_.contains(index);
    const auto& info=files_[index];
    if((info.frameLoaded && !info.thumbBGRA.empty() && (!sampleVideo || info.samples.size()==5 || info.duration100ns<=0 || FAILED(info.error))) ||
       info.corrupted || pendingPreviews_.contains(index) || pendingPreviews_.size()>=previewPool_.maxThreadCount()*2) return;
    // One finished attempt per file and scan (one more if samples are wanted later): a file that yields no picture must not be decoded in a loop.
    if(auto run=previewRuns_.constFind(index); run!=previewRuns_.cend() && (*run || !sampleVideo)) return;
    pendingPreviews_.insert(index);
    auto clock=std::make_shared<QElapsedTimer>(); clock->start();
    auto watcher=new QFutureWatcher<VideoFileInfo>(this); auto file=files_[index]; auto generation=generation_;
    connect(watcher,&QFutureWatcher<VideoFileInfo>::finished,this,[this,watcher,index,generation,sampleVideo,clock] {
        auto result=watcher->result(); watcher->deleteLater();
        trace(qPrintable("preview ready: "+QString::fromStdWString(result.fileName)),*clock);
        if(generation!=generation_ || closing_) return;
        pendingPreviews_.remove(index); previewRuns_.insert(index,sampleVideo);
        if(fileOperationPending_) return;
        if(index>=static_cast<int>(files_.size()) || files_[index].deleted || result.fullPath!=files_[index].fullPath || result.identity!=files_[index].identity) return;
        result.protectedFile=files_[index].protectedFile; result.protectedByFolder=files_[index].protectedByFolder; files_[index]=std::move(result);
        if(left_->fileIndex==index) left_->setFile(&files_[index],index); if(right_->fileIndex==index) right_->setFile(&files_[index],index);
        results_->viewport()->update(); // rows read files_ when painted
        updateSimilarity(); requestVisiblePreviews();
    });
    auto cachePath=cachePath_;
    watcher->setFuture(QtConcurrent::run(&previewPool_,[file=std::move(file),cachePath,sampleVideo]() mutable {
        Platform::ComInit com(Platform::ComApartment::MultiThreaded);
        try {
            auto identity=FileOperations::ReadIdentity(file.fullPath);
            if(identity!=file.identity) { file.corrupted=true; file.error=HRESULT_FROM_WIN32(ERROR_FILE_INVALID); }
            else {
                if(StillImages::supported(file.fullPath)) { std::atomic<bool> cancel{false}; StillImages::analyze(file,cancel); }
                else {
                    // Cached evidence keeps signatures but not thumbnails, so a cached frameLoaded must not skip decoding the picture.
                    if(file.thumbBGRA.empty()) file.frameLoaded=false;
                    VideoScanner::ExtractMetadataOnly(file); VideoScanner::ExtractMetadataAndFrame(file,sampleVideo);
                }
                if(FileOperations::ReadIdentity(file.fullPath)!=identity) { file.corrupted=true; file.error=HRESULT_FROM_WIN32(ERROR_FILE_INVALID); }
                else { ScanCache cache(cachePath); cache.store({file}); }
            }
        } catch(...) { file.error=E_FAIL; file.corrupted=true; }
        return file;
    }));
}
