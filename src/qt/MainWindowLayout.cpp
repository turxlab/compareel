#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "ResultsDelegate.h"
#include "Language.h"
#include "Icons.h"
#include "Theme.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMenuBar>
#include <QPixmapCache>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QToolButton>
#include <QVBoxLayout>

using Language::text;

void MainWindow::createLayout() {
    // Result sizes sit in the status bar so the result toolbar keeps its room for filtering at any window width.
#ifdef Q_OS_MACOS
    statusBar()->setSizeGripEnabled(false); // macOS windows resize from any edge and show no grip.
#endif
    spaceSummary_=new QLabel(statusBar()); spaceSummary_->setContentsMargins(0,0,16,0); statusBar()->addPermanentWidget(spaceSummary_); spaceSummary_->hide();
    scanTimeLabel_=new QLabel(statusBar()); scanTimeLabel_->setObjectName("scanTime");
    scanTimeLabel_->setAlignment(Qt::AlignRight|Qt::AlignVCenter); statusBar()->addPermanentWidget(scanTimeLabel_);
    // A long message can be cut off by the sizes on the right; hovering the status bar shows all of it.
    connect(statusBar(),&QStatusBar::messageChanged,this,[this](const QString& message) { statusBar()->setToolTip(message); });
    auto root=new QWidget(this); auto layout=new QVBoxLayout(root); layout->setContentsMargins(12,8,12,8); layout->setSpacing(8); setCentralWidget(root);
    auto heading=new QHBoxLayout; heading->setSpacing(8);
    // Toolbar reads left to right as one sentence: these sources, this media, this mode, then Scan.
    auto folders=new QToolButton(root); folders->setDefaultAction(foldersAction_); folders->setToolButtonStyle(Qt::ToolButtonTextBesideIcon); folders->setObjectName("sourcesButton"); heading->addWidget(folders);
    folderSummary_=new QLabel(root); heading->addWidget(folderSummary_); heading->addSpacing(8);
    mediaMode_=new QComboBox(root); mediaMode_->setObjectName("mediaMode"); mediaMode_->addItems({text("Videos"),text("Images")}); mediaMode_->setCurrentIndex(options_.images?1:0); mediaMode_->setSizeAdjustPolicy(QComboBox::AdjustToContents); heading->addWidget(mediaMode_);
    profile_=new QComboBox(root); profile_->setObjectName("scanProfile"); profile_->setMinimumWidth(172); profile_->setSizeAdjustPolicy(QComboBox::AdjustToContents); heading->addWidget(profile_);
    auto criteriaButton=tool(root,Glyph::Settings,"Scan rules…",[this] { advancedSettings(); }); criteriaButton->setObjectName("criteriaSettingsButton"); heading->addWidget(criteriaButton);
    similarityThreshold_=new QSpinBox(root); similarityThreshold_->setObjectName("similarityThreshold");
    similarityThreshold_->setRange(70,100); similarityThreshold_->setValue(static_cast<int>(std::lround(options_.minFrameSimilarity)));
    similarityThreshold_->setFixedHeight(28);
    heading->addWidget(similarityThreshold_);
    auto rescanButton=new QToolButton(root); rescanButton->setObjectName("primaryButton"); rescanButton->setDefaultAction(rescanAction_); rescanButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon); heading->addWidget(rescanButton);
    folderSummary_->setObjectName("secondary"); folderSummary_->setTextFormat(Qt::RichText); folderSummary_->setCursor(Qt::PointingHandCursor);
    connect(folderSummary_,&QLabel::linkActivated,this,[this] { chooseFolders(); });
    heading->addStretch();
    // Pausing keeps what was read and resumes later, so it sits next to Stop instead of only in the File menu.
    auto pauseButton=new QToolButton(root); pauseButton->setObjectName("pauseButton"); pauseButton->setDefaultAction(pauseAction_); pauseButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon); pauseButton->setFixedHeight(28); pauseButton->hide(); heading->addWidget(pauseButton);
    connect(pauseAction_,&QAction::changed,pauseButton,[this,pauseButton] { pauseButton->setVisible(pauseAction_->isEnabled()); });
    auto stopButton=new QToolButton(root); stopButton->setObjectName("stopButton"); stopButton->setDefaultAction(stopAction_); stopButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon); stopButton->setFixedHeight(28); stopButton->hide(); heading->addWidget(stopButton);
    connect(stopAction_,&QAction::changed,stopButton,[this,stopButton] { stopButton->setVisible(stopAction_->isEnabled()); });
    auto privacyButton=new QToolButton(root); privacyButton->setObjectName("privacyButton"); privacyButton->setDefaultAction(privacyAction_); heading->addWidget(privacyButton);
    // The two toolbar icons that are not self-explanatory get a one-word label; the panel icons (lock, folder, trash) stay icon-only.
    for(auto button:{criteriaButton,privacyButton}) { button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon); button->setMinimumSize(0,28); button->setMaximumSize(QWIDGETSIZE_MAX,28); }

    layout->addLayout(heading);
    progress_=new QProgressBar(root); progress_->setTextVisible(false); progress_->setFixedHeight(3); progress_->hide(); layout->addWidget(progress_);
    vertical_=new QSplitter(Qt::Vertical,root); vertical_->setChildrenCollapsible(false);
    auto comparison=new QWidget(vertical_); auto comparisonLayout=new QVBoxLayout(comparison); comparisonLayout->setContentsMargins(0,0,0,0); comparisonLayout->setSpacing(6);
    auto pair=new QHBoxLayout; pair->setSpacing(4); left_=new PlayerPanel(true,&previewPool_,comparison); right_=new PlayerPanel(false,&previewPool_,comparison);
    left_->canvas->setComparison(right_->canvas,0); right_->canvas->setComparison(left_->canvas,0);
    auto bridge=new QWidget(comparison); bridge->setObjectName("playbackBridge"); bridge->setFixedWidth(32);
    auto link=new QToolButton(bridge); link->setObjectName("linkControls"); link->setDefaultAction(linkAction_); link->setToolButtonStyle(Qt::ToolButtonIconOnly); link->setIconSize(QSize(18,18)); link->setFixedSize(28,28);
    // The link button sits between the two seek bars; it follows the left seek bar instead of a precomputed spacer.
    if(auto seek=left_->findChild<QSlider*>("playerSeek")) { seek->installEventFilter(this); left_->installEventFilter(this); }
    bridge->installEventFilter(this);
    pair->addWidget(left_,1); pair->addWidget(bridge); pair->addWidget(right_,1);
    auto pairArea=new QWidget(comparison); pairArea->setObjectName("pairArea"); pair->setContentsMargins(0,0,0,0); pairArea->setLayout(pair);
    // "Left" and "right" file are part of the wording, and playback controls are never mirrored, so the pair stays left-to-right in Arabic or Persian too.
    pairArea->setLayoutDirection(Qt::LeftToRight);
    left_->setParent(pairArea); bridge->setParent(pairArea); right_->setParent(pairArea); comparisonLayout->addWidget(pairArea,1);
    left_->canvas->setMinimumHeight(120); right_->canvas->setMinimumHeight(120);
    // First-run and no-results state: a complete scan form (sources, media, mode, Scan) instead of two empty players.
    // Its choices mirror the toolbar, so either place can be used.
    // While it is shown the toolbar is hidden (see updateEmptyState), so this card is the only place to start from.
    emptyState_=new QWidget(comparison); emptyState_->setObjectName("emptyState");
    {
        auto outer=new QVBoxLayout(emptyState_); outer->setContentsMargins(24,24,24,24); outer->addStretch(2);
        auto card=new QWidget(emptyState_); card->setObjectName("emptyCard"); card->setMaximumWidth(640); card->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Maximum);
        auto cardRow=new QHBoxLayout; cardRow->addStretch(); cardRow->addWidget(card,1); cardRow->addStretch(); outer->addLayout(cardRow); outer->addStretch(3);
        auto emptyLayout=new QVBoxLayout(card); emptyLayout->setContentsMargins(32,28,32,28); emptyLayout->setSpacing(14);
        auto brand=new QHBoxLayout; brand->setSpacing(12);
        auto logo=new QLabel(card); logo->setPixmap(QIcon(":/branding/app.ico").pixmap(QSize(40,40),devicePixelRatioF())); brand->addWidget(logo);
        auto titles=new QVBoxLayout; titles->setSpacing(2);
        auto title=new QLabel(card); title->setObjectName("emptyTitle"); title->setWordWrap(true); titles->addWidget(title);
        auto hint=new QLabel(card); hint->setObjectName("emptyHint"); hint->setWordWrap(true); titles->addWidget(hint);
        brand->addLayout(titles,1); emptyLayout->addLayout(brand); emptyLayout->addSpacing(4);
        // Step 1: sources, as a drop target that also opens the chooser.
        auto drop=new QWidget(card); drop->setObjectName("emptyDropZone"); auto dropLayout=new QHBoxLayout(drop); dropLayout->setContentsMargins(16,14,16,14); dropLayout->setSpacing(12);
        auto folderIcon=new QLabel(drop); folderIcon->setObjectName("emptyFolderIcon"); folderIcon->setPixmap(icon(Glyph::Folder,"@iconAccent").pixmap(QSize(26,26),devicePixelRatioF())); dropLayout->addWidget(folderIcon);
        auto sourceText=new QVBoxLayout; sourceText->setSpacing(2);
        auto sourcesLabel=new QLabel(drop); sourcesLabel->setObjectName("emptyFieldLabel"); sourcesLabel->setProperty("key","Sources · drop folders or files here"); sourcesLabel->setWordWrap(true); sourceText->addWidget(sourcesLabel);
        // Plain text: the Choose/Change button beside it is the one way to open the source list.
        auto sources=new QLabel(drop); sources->setObjectName("emptySources"); sources->setTextFormat(Qt::PlainText); sourceText->addWidget(sources);
        dropLayout->addLayout(sourceText,1);
        auto choose=new QToolButton(drop); choose->setObjectName("emptySourcesButton"); choose->setToolButtonStyle(Qt::ToolButtonTextOnly);
        connect(choose,&QToolButton::clicked,this,[this] { chooseFolders(); }); dropLayout->addWidget(choose);
        emptyLayout->addWidget(drop);
        // Step 2: what to look for, with the detailed rules one click away.
        auto findRow=new QHBoxLayout; findRow->setSpacing(8);
        auto findLabel=new QLabel(card); findLabel->setObjectName("emptyFieldLabel"); findLabel->setProperty("key","Find"); findRow->addWidget(findLabel);
        emptyMedia_=new QComboBox(card); emptyMedia_->setObjectName("emptyMediaMode"); emptyMedia_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
        emptyProfile_=new QComboBox(card); emptyProfile_->setObjectName("emptyScanProfile"); emptyProfile_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
        connect(emptyMedia_,&QComboBox::activated,this,[this](int index) { mediaMode_->setCurrentIndex(index); syncEmptyStateChoices(); });
        connect(emptyProfile_,&QComboBox::activated,this,[this](int index) { profile_->setCurrentIndex(index); syncEmptyStateChoices(); if(index==2) advancedSettings(); });
        auto rules=tool(card,Glyph::Settings,"Scan rules…",[this] { advancedSettings(); }); rules->setObjectName("emptyCriteriaButton"); rules->setToolButtonStyle(Qt::ToolButtonTextBesideIcon); rules->setMinimumSize(0,28); rules->setMaximumSize(QWIDGETSIZE_MAX,28);
        // Controls keep their natural width; only the drop zone spans the card.
        findRow->addWidget(emptyMedia_); findRow->addWidget(emptyProfile_); findRow->addWidget(rules); findRow->addStretch(); emptyLayout->addLayout(findRow);
        emptyLayout->addSpacing(4);
        // Everything in the card is left-aligned; the footer puts the other ways in on the left and the primary action on the right, as in a dialog.
        auto footer=new QHBoxLayout; footer->setSpacing(18);
        // Text links rather than flat buttons: a button's frame margin would push them off the card's left edge.
        auto textLink=[card](QAction* target) {
            auto label=new QLabel(card); label->setObjectName("emptyLink");
            label->setTextInteractionFlags(Qt::LinksAccessibleByMouse|Qt::LinksAccessibleByKeyboard); label->setFocusPolicy(Qt::TabFocus);
            connect(label,&QLabel::linkActivated,target,[target] { target->trigger(); });
            auto refresh=[label,target] {
                label->setText(QString("<a href=\"action\" style=\"color:%2; text-decoration:none\">%1</a>").arg(target->text().toHtmlEscaped(),Theme::color("@link").name()));
                label->setAccessibleName(target->text()); label->setVisible(target->isEnabled());
            };
            connect(target,&QAction::changed,label,refresh); refresh();
            return label;
        };
        auto scan=new QToolButton(card); scan->setObjectName("emptyScanButton"); scan->setDefaultAction(rescanAction_); scan->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        footer->addWidget(textLink(findChild<QAction*>("Open two files…"))); footer->addWidget(textLink(resumeReviewAction_)); footer->addStretch(); footer->addWidget(scan); emptyLayout->addLayout(footer);
    }
    comparisonLayout->addWidget(emptyState_,1); emptyState_->hide();
    left_->canvas->transformChanged=[this](double zoom,QPointF pan) { right_->canvas->setTransform(zoom,pan); };
    right_->canvas->transformChanged=[this](double zoom,QPointF pan) { left_->canvas->setTransform(zoom,pan); };
    for(auto panel:{left_,right_}) {
        panel->canvas->animationHold=[this,panel](bool held) {
            if(!panel->animationReady()) return false;
            if(held && (hideVideosAction_->isChecked() || scanning_ || fileOperationPending_)) return true;
            for(auto target:{left_,right_}) { target->playImage(false); target->seekImage(0); if(held) target->playImage(true); }
            return true;
        };
        panel->imageSeekRequested=[this,panel](int value) {
            panel->playImage(false);
            if(panel->imageDocument()) panel->showImageIndex(value);
            else { panel->seekImage(panel->imageDuration()*value/10000); if(linkAction_->isChecked()) { auto other=panel==left_?right_:left_; other->playImage(false); other->seekImage(panel->imageTime()); } }
        };
        panel->imageStepRequested=[this,panel](bool forward) {
            panel->stepImage(forward);
            if(panel->animationReady() && linkAction_->isChecked()) { auto other=panel==left_?right_:left_; other->playImage(false); other->seekImage(panel->imageTime()); }
        };
        panel->seekRequested=[this,panel](int64_t position,bool exact) { controlSeek(panel,position,exact); };
        panel->playRequested=[this,panel] {
            if(options_.images) {
                const bool playing=!panel->imagePlaying();
                if(linkAction_->isChecked()) { left_->playImage(playing); right_->playImage(playing); }
                else panel->playImage(playing);
                return;
            }
            lastControlledRight_=panel==right_;
            if(linkAction_->isChecked()) playBoth(panel); else panel->player->TogglePlayPause();
        };
        panel->stepRequested=[this,panel](bool forward) {
            lastControlledRight_=panel==right_; syncing_=false;
            if(linkAction_->isChecked()) { left_->player->Pause(); right_->player->Pause(); linkedStepFromRight_=lastControlledRight_; }
            panel->player->StepFrame(forward);
        };
    }
    left_->removeRequested=[this] { deleteFile(left_); }; right_->removeRequested=[this] { deleteFile(right_); };
    left_->detailsRequested=[this] { showFileDetails(left_); }; right_->detailsRequested=[this] { showFileDetails(right_); };
    left_->protectRequested=[this] { toggleProtection(left_); }; right_->protectRequested=[this] { toggleProtection(right_); };
    auto pairInfoArea=new QWidget(comparison); pairInfoArea->setObjectName("pairInfo");
    auto pairInfo=new QHBoxLayout(pairInfoArea); pairInfo->setContentsMargins(0,0,0,0);
    similarity_=new QLabel(comparison); similarity_->setObjectName("pairSimilarity"); similarity_->setWordWrap(false); similarity_->setMinimumWidth(1); similarity_->setText(text("No pair selected")); similarity_->setProperty("verdict","none"); similarity_->installEventFilter(this);
    // The chip grows up to its text width (setVerdict sets the maximum); the stretch after it takes the rest.
    similarity_->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Preferred); pairInfo->addWidget(similarity_,1); pairInfo->addStretch(0);
    auto autoAlign=tool(comparison,Glyph::Align,"Auto-align partial clip",[this] { autoAlignPartialClip(); }); autoAlign->setObjectName("autoAlignPartialClip"); autoAlign->setFixedSize(28,28); autoAlign->setEnabled(false); pairInfo->addWidget(autoAlign);
    auto alignmentPanel=new QWidget(comparison); alignmentPanel->setObjectName("alignmentControls"); alignmentPanel->setEnabled(false);
    auto alignmentLayout=new QHBoxLayout(alignmentPanel); alignmentLayout->setContentsMargins(0,0,0,0); alignmentLayout->setSpacing(6); alignmentLayout->setSizeConstraint(QLayout::SetFixedSize);
    auto offsetLabel=new QLabel(alignmentPanel); offsetLabel->setObjectName("offsetLabel"); alignmentLayout->addWidget(offsetLabel);
    offset_=new QDoubleSpinBox(alignmentPanel); offset_->setObjectName("alignmentOffset"); offset_->setRange(-3600,3600); offset_->setDecimals(3); offset_->setSingleStep(0.1); offset_->setSuffix(" s"); offset_->setFixedHeight(28);
    offset_->setFixedWidth(offset_->sizeHint().width()+8); alignmentLayout->addWidget(offset_);
    auto resetOffset=tool(alignmentPanel,Glyph::RestartVideo,"Reset alignment",[this] { offset_->setValue(0); }); resetOffset->setObjectName("resetAlignment"); resetOffset->setFixedSize(28,28); alignmentLayout->addWidget(resetOffset);
    pairInfo->addWidget(alignmentPanel); comparisonLayout->addWidget(pairInfoArea);
    // Per-pair review decisions are the core action, so they get visible buttons instead of a submenu.
    reviewBar_=new QWidget(comparison); reviewBar_->setObjectName("reviewBar");
    {
        auto bar=new QHBoxLayout(reviewBar_); bar->setContentsMargins(0,0,0,0); bar->setSpacing(4);
        // The three decisions are one segmented choice; the actions after the divider change files or results.
        auto segment=new QHBoxLayout; segment->setSpacing(0);
        for(int i=1;i<4;++i) {
            auto button=new QToolButton(reviewBar_); button->setObjectName("decisionButton"); button->setDefaultAction(reviewActions_[i]); button->setToolButtonStyle(Qt::ToolButtonTextOnly);
            button->setProperty("segment",i==1?"first":i==3?"last":"middle"); segment->addWidget(button);
        }
        bar->addLayout(segment); bar->addSpacing(8);
        auto divider=new QFrame(reviewBar_); divider->setObjectName("panelSeparator"); divider->setFixedSize(1,22); bar->addWidget(divider); bar->addSpacing(8);
        markRightButton_=new QToolButton(reviewBar_); markRightButton_->setObjectName("reviewButton"); markRightButton_->setDefaultAction(findChild<QAction*>("Mark right file for cleanup")); markRightButton_->setToolButtonStyle(Qt::ToolButtonTextOnly); bar->addWidget(markRightButton_);
        auto ignore=new QToolButton(reviewBar_); ignore->setObjectName("reviewButton"); ignore->setDefaultAction(findChild<QAction*>("Ignore this pair")); ignore->setToolButtonStyle(Qt::ToolButtonTextOnly); bar->addWidget(ignore);
        // Marked files are removed from here too, not only from the Review menu; the button appears once something is marked.
        cleanupMarkedButton_=new QToolButton(reviewBar_); cleanupMarkedButton_->setObjectName("reviewButton"); cleanupMarkedButton_->setToolButtonStyle(Qt::ToolButtonTextOnly); cleanupMarkedButton_->hide();
        connect(cleanupMarkedButton_,&QToolButton::clicked,this,[this] { removeMarkedFiles(); }); bar->addWidget(cleanupMarkedButton_);
        bar->addStretch(1);
        auto nextPair=new QToolButton(reviewBar_); nextPair->setObjectName("reviewButton"); nextPair->setDefaultAction(findChild<QAction*>("Next pair")); nextPair->setToolButtonStyle(Qt::ToolButtonTextBesideIcon); nextPair->setIcon(icon(Glyph::NextGroup)); bar->addWidget(nextPair);
    }
    comparisonLayout->addWidget(reviewBar_);
    connect(offset_,&QDoubleSpinBox::valueChanged,this,[this](double seconds) {
        alignmentOffset100ns_=static_cast<int64_t>(std::llround(seconds*10000000)); seekBoth(left_->player->GetPosition100ns(),true);
        findChild<QToolButton*>("resetAlignment")->setEnabled(seconds!=0);
    });
    vertical_->addWidget(comparison);
    auto resultArea=new QWidget(vertical_); resultArea->setObjectName("resultArea"); auto resultLayout=new QVBoxLayout(resultArea); resultLayout->setContentsMargins(0,4,0,0); resultLayout->setSpacing(6);
    auto resultBar=new QHBoxLayout; resultBar->setSpacing(6);
    auto previous=new QToolButton(resultArea); previous->setObjectName("previousGroupButton"); previous->setDefaultAction(previousAction_); resultBar->addWidget(previous);
    // Labels and combos on this row shrink (eliding) instead of widening the window past the screen in longer languages.
    groupLabel_=new ElidedLabel(resultArea); groupLabel_->setObjectName("groupPosition"); resultBar->addWidget(groupLabel_);
    auto next=new QToolButton(resultArea); next->setObjectName("nextGroupButton"); next->setDefaultAction(nextAction_); resultBar->addWidget(next);
    resultSummary_=new ElidedLabel(resultArea); resultSummary_->setObjectName("secondary"); resultBar->addSpacing(8); resultBar->addWidget(resultSummary_); resultBar->addStretch();
    filter_=new QLineEdit(resultArea); filter_->setObjectName("resultFilter"); filter_->setClearButtonEnabled(true); filter_->addAction(icon(Glyph::Search,"@iconMuted"),QLineEdit::LeadingPosition); filter_->setMaximumWidth(260); filter_->setMinimumWidth(80); resultBar->addWidget(filter_);
    evidenceFilter_=new QComboBox(resultArea); evidenceFilter_->setObjectName("evidenceFilter"); evidenceFilter_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); evidenceFilter_->setMinimumContentsLength(14);
    for(auto key:{"All matches","Exact copies","Includes exact copies","Similar matches"}) evidenceFilter_->addItem(text(key));
    evidenceFilter_->setToolTip(text("Exact copies: every file in the group has the same verified content. Includes exact copies: at least two files in the group are identical."));
    resultBar->addWidget(evidenceFilter_); connect(evidenceFilter_,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this] { applyFilter(); });
    // Two separate questions: which file of each group is the reference, and in which order the groups are listed.
    auto referenceLabel=new QLabel(resultArea); referenceLabel->setObjectName("referenceRuleLabel"); resultBar->addSpacing(6); resultBar->addWidget(referenceLabel);
    referenceRule_=new QComboBox(resultArea); referenceRule_->setObjectName("referenceRule"); referenceRule_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); referenceRule_->setMinimumContentsLength(16);
    referenceLabel->setBuddy(referenceRule_); resultBar->addWidget(referenceRule_);
    {
        // Older versions stored only a left-file preference; "largest file" is the one choice that carries over.
        const int saved=settings_.contains("referenceRule")?settings_.value("referenceRule").toInt():settings_.value("leftFilePreference",0).toInt()==1?int(ReferenceRule::LargestFile):int(ReferenceRule::HighestResolution);
        fillReferenceRules(); const int found=referenceRule_->findData(saved); referenceRule_->setCurrentIndex(found>=0?found:0);
    }
    connect(referenceRule_,&QComboBox::currentIndexChanged,this,[this] { applyReferenceRule(); });
    auto sortLabel=new QLabel(resultArea); sortLabel->setObjectName("sortLabel"); resultBar->addSpacing(6); resultBar->addWidget(sortLabel);
    sort_=new QComboBox(resultArea); sort_->setObjectName("resultSort"); sort_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); sort_->setMinimumContentsLength(16); sortLabel->setBuddy(sort_); resultBar->addWidget(sort_); resultLayout->addLayout(resultBar);
    QWidget::setTabOrder(folders,mediaMode_); QWidget::setTabOrder(mediaMode_,profile_); QWidget::setTabOrder(profile_,similarityThreshold_); QWidget::setTabOrder(similarityThreshold_,rescanButton);
    QWidget::setTabOrder(rescanButton,stopButton); QWidget::setTabOrder(stopButton,privacyButton);
    if(auto leftFirst=left_->findChild<QLineEdit*>("filename")) QWidget::setTabOrder(privacyButton,leftFirst);
    if(auto leftLast=left_->findChild<QToolButton*>("fileDetails")) QWidget::setTabOrder(leftLast,link);
    if(auto rightFirst=right_->findChild<QLineEdit*>("filename")) QWidget::setTabOrder(link,rightFirst);
    if(auto rightLast=right_->findChild<QToolButton*>("fileDetails")) QWidget::setTabOrder(rightLast,autoAlign);
    QWidget::setTabOrder(autoAlign,offset_); QWidget::setTabOrder(offset_,resetOffset);
    // The review bar sits between the pair and the result list, so Tab reaches its buttons in that place, left to right.
    { QWidget* chain=resetOffset;
      for(auto widget:reviewBar_->findChildren<QWidget*>(QString(),Qt::FindDirectChildrenOnly)) if(widget->focusPolicy()&Qt::TabFocus) { QWidget::setTabOrder(chain,widget); chain=widget; }
      QWidget::setTabOrder(chain,previous); }
    QWidget::setTabOrder(previous,next);
    QWidget::setTabOrder(next,filter_); QWidget::setTabOrder(filter_,evidenceFilter_); QWidget::setTabOrder(evidenceFilter_,referenceRule_); QWidget::setTabOrder(referenceRule_,sort_);
    results_=new QTreeWidget(resultArea); results_->setObjectName("results"); results_->setColumnCount(1); results_->setHeaderHidden(true);
    // Thumbnails run reference-first from the left like the players above them, in every language.
    results_->setLayoutDirection(Qt::LeftToRight);
    results_->setAlternatingRowColors(false); results_->setRootIsDecorated(false); results_->setItemsExpandable(false); results_->setIndentation(0);
    results_->setSelectionMode(QAbstractItemView::SingleSelection); results_->setUniformRowHeights(true);
    results_->setItemDelegate(new ResultsDelegate(this)); results_->viewport()->setMouseTracking(true);
    results_->viewport()->installEventFilter(this);
    results_->installEventFilter(this);
    // The comparison is the primary surface: it takes extra height first, the result list keeps at least one row.
    results_->setMinimumHeight(126); resultLayout->addWidget(results_,1); vertical_->addWidget(resultArea); vertical_->setStretchFactor(0,1); vertical_->setStretchFactor(1,0); vertical_->setHandleWidth(11); vertical_->setSizes({560,300}); layout->addWidget(vertical_,1);
    QWidget::setTabOrder(sort_,results_);
    // Groups change only on a click or an arrow key (both handled in eventFilter). Qt also moves the current
    // row on its own, e.g. when a thumbnail inside another row takes focus; that must not switch the comparison.
    connect(results_,&QTreeWidget::currentItemChanged,this,[this](QTreeWidgetItem* item) {
        if(!item || item->data(0,Qt::UserRole).toInt()==currentGroup_) return;
        QSignalBlocker blocker(results_); results_->setCurrentItem(currentGroup_>=0?results_->topLevelItem(currentGroup_):nullptr);
    });
    connect(filter_,&QLineEdit::textChanged,this,[this] { applyFilter(); });
    connect(results_->verticalScrollBar(),&QScrollBar::valueChanged,this,[this] { requestVisiblePreviews(); });
    connect(sort_,&QComboBox::currentIndexChanged,this,[this](int index) {
        if(index<0 || scanning_) return;
        options_.sortOrder=static_cast<SortOrder>(index);
        scanCriteria_.sortOrder=options_.sortOrder;
        sortResults();
    });
    connect(profile_,&QComboBox::currentIndexChanged,this,[this](int index) {
        QTimer::singleShot(0,this,[this] { syncEmptyStateChoices(); });
        profile_->setToolTip(text(index==0?"Exact copies are verified with a full SHA-256 content hash.":index==1?(options_.images?"Oriented sRGB images are compared on white. Visual similarity does not prove identical files.":"Five positions are compared. Audio and subtitle equality is not verified."):"Advanced criteria find candidates; they do not prove identical content."));
        if(similarityThreshold_) similarityThreshold_->setVisible(index==1 && !(emptyState_ && !emptyState_->isHidden()));
    });
    connect(similarityThreshold_,QOverload<int>::of(&QSpinBox::valueChanged),this,[this](int value) {
        options_.minFrameSimilarity=value;
        saveSettings();
    });
    connect(profile_,&QComboBox::activated,this,[this](int index) {
        if(index==2) advancedSettings();
    });
    connect(mediaMode_,&QComboBox::currentIndexChanged,this,[this](int index) {
        if(scanning_ || sortingMetadata_ || fileOperationPending_ || closing_) { QSignalBlocker blocker(mediaMode_); mediaMode_->setCurrentIndex(options_.images?1:0); return; }
        if(options_.images==(index==1)) return;
        settings_.setValue(options_.images?"imageExtensions":"extensions",strings(options_.extensions));
        settings_.setValue(options_.images?"imageExcludedExtensions":"excludedExtensions",strings(options_.excludedExtensions));
        options_.images=index==1;
        options_.extensions=paths(settings_.value(options_.images?"imageExtensions":"extensions").toStringList());
        options_.excludedExtensions=paths(settings_.value(options_.images?"imageExcludedExtensions":"excludedExtensions").toStringList());
        ++generation_; syncing_=false; linkedStepFromRight_.reset(); restorePositions_.reset(); pendingPreviews_.clear(); previewRuns_.clear(); requestedSamples_.clear(); markedFiles_.clear();
        left_->setFile(nullptr,-1); right_->setFile(nullptr,-1); files_.clear(); groups_.clear(); manual_=false; currentGroup_=-1;
        if(options_.images && (options_.sortOrder==SortOrder::DurationAsc || options_.sortOrder==SortOrder::DurationDesc)) options_.sortOrder=SortOrder::NameAsc;
        updateMediaMode(); translate(); populateResults(); updateSimilarity(); saveSettings();
    });
}
void MainWindow::updateMediaMode() {
    const bool images=options_.images;
    left_->setMediaMode(images); right_->setMediaMode(images);
    findChild<QWidget*>("playbackBridge")->setVisible(!images); offset_->parentWidget()->setVisible(!images);
    if(auto autoAlign=findChild<QToolButton*>("autoAlignPartialClip")) autoAlign->setVisible(!images);
    if(auto autoAlignAct=findChild<QAction*>("Auto-align partial clip")) autoAlignAct->setEnabled(!images);
    playBothAction_->setEnabled(!images); linkAction_->setEnabled(!images);
    findChild<QAction*>("Restart both")->setEnabled(!images);
    if(auto model=qobject_cast<QStandardItemModel*>(sort_->model())) for(int index:{2,3}) if(auto item=model->item(index)) item->setEnabled(!images);
    if(auto view=qobject_cast<QListView*>(sort_->view())) for(int index:{2,3}) view->setRowHidden(index,images);
    if(images && (options_.sortOrder==SortOrder::DurationAsc || options_.sortOrder==SortOrder::DurationDesc)) {
        options_.sortOrder=SortOrder::NameAsc; QSignalBlocker blocker(sort_); sort_->setCurrentIndex(int(options_.sortOrder));
    }
    { QSignalBlocker blocker(mediaMode_); mediaMode_->setCurrentIndex(images?1:0); }
}

void MainWindow::translate() {
    // Tooltips carry the shortcut so icon-only buttons teach their keyboard path.
    for(const auto& [item,key]:actionTexts_) { item->setText(text(key)); item->setToolTip(item->shortcut().isEmpty()?text(key):text(key)+"  ("+item->shortcut().toString(QKeySequence::NativeText)+")"); }
    if(options_.images) hideVideosAction_->setText(text("Hide both image previews"));
    privacyAction_->setIconText(text("Privacy"));
    for(auto name:{"criteriaSettingsButton","emptyCriteriaButton"}) if(auto button=findChild<QToolButton*>(name)) {
        button->setText(text("Rules")); button->setToolTip(text("Scan rules…")+nativeShortcuts("  (Ctrl+Shift+,)")); button->setAccessibleName(text("Scan rules…"));
    }
    for(auto menu:menuBar()->findChildren<QMenu*>()) if(!menu->objectName().isEmpty()) menu->setTitle(text(menu->objectName().toUtf8().constData()));
    int profileIndex=std::max(0,profile_->currentIndex()); QSignalBlocker profileBlock(profile_); profile_->clear();
    for(auto label:{"Exact copies",options_.images?"Similar images":"Similar videos","Advanced criteria"}) profile_->addItem(text(label)); profile_->setCurrentIndex(profileIndex);
    mediaMode_->setItemText(0,text("Videos")); mediaMode_->setItemText(1,text("Images"));
    profile_->setToolTip(text(profileIndex==0?"Exact copies are verified with a full SHA-256 content hash.":profileIndex==1?(options_.images?"Oriented sRGB images are compared on white. Visual similarity does not prove identical files.":"Five positions are compared. Audio and subtitle equality is not verified."):"Advanced criteria find candidates; they do not prove identical content."));
    QSignalBlocker sortBlock(sort_); sort_->clear(); for(auto label:{"Largest first","Smallest first","Longest first","Shortest first","Highest resolution","Lowest resolution","Filename"}) sort_->addItem(text(label)); sort_->setCurrentIndex(static_cast<int>(options_.sortOrder));
    fillReferenceRules();
    // Plain wording for the rule; "reference" stays the name of the left file in the players.
    if(auto label=findChild<QLabel*>("referenceRuleLabel")) label->setText(text("Keep in each group"));
    if(auto label=findChild<QLabel*>("sortLabel")) label->setText(text("Sort groups"));
    sort_->setAccessibleName(text("Sort groups"));
    filter_->setPlaceholderText(text("Filter…")); filter_->setToolTip(text("Filter by filename or path…")); filter_->setAccessibleName(text("Filter by filename or path…"));
    if(evidenceFilter_) { int i=0; for(auto key:{"All matches","Exact copies","Includes exact copies","Similar matches"}) evidenceFilter_->setItemText(i++,text(key)); evidenceFilter_->setAccessibleName(text("All matches")); }
    for(auto canvas:{left_->canvas,right_->canvas}) canvas->setToolTip(text("Hold B on a preview to show the other file; release to return."));
    offset_->setToolTip(text("Right video offset in seconds. Positive values show a later moment on the right.")); offset_->setAccessibleName(text("Right video offset"));
    updateSourceSummary();
    linkAction_->setToolTip(nativeShortcuts(text(linkAction_->isChecked()?"Controls linked · Ctrl+L to unlink":"Independent controls · Ctrl+L to link")));
    findChild<QLabel*>("offsetLabel")->setText(text("Offset"));
    auto resetAlignment=findChild<QToolButton*>("resetAlignment"); resetAlignment->setToolTip(text("Reset alignment")); resetAlignment->setAccessibleName(text("Reset alignment"));
    for(auto button:findChildren<QToolButton*>()) if(auto act=button->defaultAction()) {
        button->setAccessibleName(act->text());
        if(button->objectName()!="linkControls") button->setToolTip(act->toolTip());
    }
    if(auto link=findChild<QToolButton*>("linkControls")) link->setToolTip(linkAction_->toolTip());
    mediaMode_->setAccessibleName(text("Media type")); mediaMode_->setToolTip(text("Media type"));
    profile_->setAccessibleName(text("Comparison mode"));
    if(similarityThreshold_) {
        QSignalBlocker block(similarityThreshold_);
        // The spin box value sits where %1 is, so each language can place the percent sign on either side.
        const auto format=text("Similarity \u2265 %1%"); const auto at=format.indexOf("%1");
        similarityThreshold_->setPrefix(at<0?format:format.left(at)); similarityThreshold_->setSuffix(at<0?QString():format.mid(at+2));
        similarityThreshold_->setValue(static_cast<int>(std::lround(options_.minFrameSimilarity)));
        similarityThreshold_->setToolTip(text("Lower thresholds find more candidates and may increase false matches."));
        similarityThreshold_->setAccessibleName(text("Similarity threshold"));
        similarityThreshold_->setVisible(profile_->currentIndex()==1);
    }
    filter_->setToolTip(text("Filter by filename or path…"));
    sort_->setAccessibleName(text("Sort groups by")); sort_->setToolTip(text("Sort groups by"));
    results_->setAccessibleName(text("Matches"));
    if(auto autoAlign=findChild<QToolButton*>("autoAlignPartialClip")) {
        autoAlign->setToolTip(text("Automatically estimate time offset when two videos share a trimmed or common sub-segment."));
        autoAlign->setAccessibleName(text("Auto-align partial clip"));
    }
    // Arabic and Persian mirror the bars: arrows point the way they move, and the segmented decision buttons swap their rounded ends.
    const bool rtl=QGuiApplication::isRightToLeft();
    previousAction_->setIcon(icon(rtl?Glyph::NextGroup:Glyph::PreviousGroup)); nextAction_->setIcon(icon(rtl?Glyph::PreviousGroup:Glyph::NextGroup));
    for(auto button:reviewBar_->findChildren<QToolButton*>()) {
        if(button->defaultAction()==findChild<QAction*>("Next pair")) button->setIcon(icon(rtl?Glyph::PreviousGroup:Glyph::NextGroup));
        for(int i:{1,3}) if(button->defaultAction()==reviewActions_[i]) {
            button->setProperty("segment",(i==1)!=rtl?"first":"last"); button->style()->unpolish(button); button->style()->polish(button);
        }
    }
    left_->translate(); right_->translate();
    if(currentGroup_>=0 && currentGroup_<static_cast<int>(groups_.size())) right_->setCandidates(&groups_[currentGroup_]);
    resultSummary_->setText(text("Matches"));
    showGroupPosition();
    updateSimilarity(); statusBar()->showMessage(text("Ready"));
    updateScanTimeLabel(); updateMediaMode(); updateEmptyState();
}

void MainWindow::setTheme(const QString& id) {
    Theme::apply(*qApp,id); settings_.setValue("theme",Theme::current()); settings_.sync();
    // Stylesheet colours follow on their own; pixmaps and per-row styles are rebuilt here.
    if(auto folder=findChild<QLabel*>("emptyFolderIcon")) folder->setPixmap(icon(Glyph::Folder,"@iconAccent").pixmap(QSize(26,26),devicePixelRatioF()));
    // Rich-text links carry their colour inline (QLabel does not follow a palette change), so their owners re-render them.
    for(auto item:{findChild<QAction*>("Open two files…"),resumeReviewAction_}) if(item) emit item->changed();
    updateSourceSummary(); left_->updateMetadata(); right_->updateMetadata(); populateResults();
    for(auto widget:findChildren<QWidget*>()) widget->update();
}

void MainWindow::applyPrivacy() {
    left_->setImagePrivacy(hideVideosAction_->isChecked()); right_->setImagePrivacy(hideVideosAction_->isChecked());
    bool hidden=hideVideosAction_->isChecked();
    privacyAction_->setIcon(icon(hidden && hideThumbnailsAction_->isChecked()?Glyph::EyeOff:Glyph::Eye));
    privacyAction_->setToolTip(text(hidden && hideThumbnailsAction_->isChecked()?"Show all previews":"Hide all previews"));
    { QSignalBlocker blocker(privacyAction_); privacyAction_->setChecked(hidden && hideThumbnailsAction_->isChecked()); }
    if(hidden) { syncing_=false; left_->player->Pause(); right_->player->Pause(); }
    left_->canvas->setPreviewHidden(hidden); right_->canvas->setPreviewHidden(hidden);
    settings_.setValue("hideVideoPreviews",hidden); settings_.setValue("hideThumbnails",hideThumbnailsAction_->isChecked()); settings_.sync();
    results_->viewport()->update(); requestVisiblePreviews();
}

void MainWindow::updateEmptyState() {
    if(!emptyState_) return;
    const bool empty=!scanning_ && groups_.empty() && left_->fileIndex<0 && right_->fileIndex<0;
    // "No matches" describes a scan the user just finished, never one restored from a previous run.
    const bool firstRun=folders_.empty() || !scanCompletedThisSession_;
    // A scan that found no files of the chosen type says so, instead of reporting "no matches" among nothing.
    const bool noFiles=!firstRun && files_.empty();
    if(auto title=emptyState_->findChild<QLabel*>("emptyTitle")) title->setText(text(firstRun?"Find duplicate videos and images":noFiles?"No files to compare":"No matches found"));
    if(auto hint=emptyState_->findChild<QLabel*>("emptyHint")) hint->setText(firstRun
        ?text("Choose what to find, then scan.")
        :noFiles?text(options_.images?"No images were found in these sources. Check the folders, or switch to Videos if you are looking for videos."
                                      :"No videos were found in these sources. Check the folders, or switch to Images if you are looking for photos.")
        :text("Nothing matched these settings. Try another comparison mode or other sources, then scan again."));
    for(auto label:emptyState_->findChildren<QLabel*>("emptyFieldLabel")) label->setText(text(label->property("key").toByteArray().constData()));
    if(auto sources=emptyState_->findChild<QLabel*>("emptySources")) sources->setText(sourceSummary(folders_));
    if(auto choose=emptyState_->findChild<QToolButton*>("emptySourcesButton")) { choose->setText(text(folders_.empty()?"Choose…":"Change…")); choose->setEnabled(foldersAction_->isEnabled()); }
    syncEmptyStateChoices();
    const bool appearing=empty && emptyState_->isHidden();
    emptyState_->setVisible(empty);
    // Focus goes to the next expected step, so Enter does the right thing and no combo looks pre-selected.
    if(appearing) QTimer::singleShot(0,this,[this] {
        if(!emptyState_ || emptyState_->isHidden()) return;
        if(auto target=emptyState_->findChild<QToolButton*>(folders_.empty()?"emptySourcesButton":"emptyScanButton")) target->setFocus(Qt::OtherFocusReason);
    });
    for(auto name:{"pairArea","pairInfo","reviewBar","resultArea"}) if(auto widget=findChild<QWidget*>(name)) widget->setVisible(!empty);
    // The welcome card carries the same choices, so the toolbar would only repeat them.
    for(QWidget* widget:std::initializer_list<QWidget*>{findChild<QToolButton*>("sourcesButton"),folderSummary_,mediaMode_,profile_,findChild<QToolButton*>("criteriaSettingsButton"),findChild<QToolButton*>("primaryButton"),findChild<QToolButton*>("privacyButton")})
        if(widget) widget->setVisible(!empty);
    if(similarityThreshold_) similarityThreshold_->setVisible(!empty && profile_->currentIndex()==1);
}
void MainWindow::syncEmptyStateChoices() {
    if(!emptyMedia_ || !emptyProfile_) return;
    for(auto [mirror,source]:{std::pair{emptyMedia_,mediaMode_},std::pair{emptyProfile_,profile_}}) {
        QSignalBlocker blocker(mirror);
        if(mirror->count()!=source->count()) { mirror->clear(); for(int i=0;i<source->count();++i) mirror->addItem(source->itemText(i)); }
        else for(int i=0;i<source->count();++i) mirror->setItemText(i,source->itemText(i));
        mirror->setCurrentIndex(source->currentIndex()); mirror->setToolTip(source->toolTip()); mirror->setAccessibleName(source->accessibleName());
        mirror->setEnabled(source->isEnabled());
    }
}
void MainWindow::updateSourceSummary() {
    folderSummary_->setText(sourceLink(folders_));
    if(auto sources=emptyState_?emptyState_->findChild<QLabel*>("emptySources"):nullptr) sources->setText(sourceSummary(folders_));
}
