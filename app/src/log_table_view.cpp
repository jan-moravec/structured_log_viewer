#include "log_table_view.hpp"

#include "anchor_manager.hpp"
#include "jump_to_tail_pill.hpp"
#include "shortcut_catalog.hpp"

#include <log_model.hpp>

#include <QAbstractItemModel>
#include <QAbstractProxyModel>
#include <QApplication>
#include <QClipboard>
#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QMainWindow>
#include <QObject>
#include <QPaintEvent>
#include <QPainter>
#include <QPalette>
#include <QPen>
#include <QPoint>
#include <QRect>
#include <QResizeEvent>
#include <QScrollBar>
#include <QString>
#include <QStyleOptionHeader>
#include <QWheelEvent>
#include <QWidget>

#include <algorithm>
#include <utility>

namespace
{
/// Logical-pixel gap between the pill and the tail-side viewport
/// edge. Leaves room for the scrollbar's thumb / arrow indicator.
constexpr int PILL_VIEWPORT_MARGIN_PX = 12;
} // namespace

void LogHeaderView::CenterIconAlignmentForIconOnlySection(QStyleOptionHeader *option)
{
    // `iconAlignment` defaults to left-aligned, so an icon-only
    // header (theme identity icon, no text) ends up hugging the
    // section's left edge. Centre it so it lines up with the
    // centred pills below. Sections with both icon and text keep
    // the default left-of-text layout.
    if (option != nullptr && option->text.isEmpty() && !option->icon.isNull())
    {
        option->iconAlignment = Qt::AlignCenter;
    }
}

void LogHeaderView::initStyleOptionForIndex(QStyleOptionHeader *option, int logicalIndex) const
{
    QHeaderView::initStyleOptionForIndex(option, logicalIndex);
    CenterIconAlignmentForIconOnlySection(option);
}

LogTableView::LogTableView(QWidget *parent)
    : QTableView(parent)
{
    setAcceptDrops(true);

    // Custom header so icon-only sections centre their glyph;
    // Qt takes ownership and deletes the previous default header.
    setHorizontalHeader(new LogHeaderView(Qt::Horizontal, this));

    const QScrollBar *vbar = verticalScrollBar();

    // Edge-triggered scroll detection gated on `mNextValueChangeIsUser`
    // so non-user changes (programmatic `scrollTo`, `endInsertRows`
    // clamping, our anchor restore) cannot disengage Follow newest.
    connect(vbar, &QAbstractSlider::valueChanged, this, &LogTableView::OnVerticalScrollValueChanged);

    // Row inserts that grow `maximum` without moving `value` don't
    // fire `valueChanged`; the range hook keeps `mAtTailEdge`
    // fresh so a user at the previous tail does not silently fall
    // behind.
    connect(vbar, &QAbstractSlider::rangeChanged, this, &LogTableView::OnVerticalScrollRangeChanged);

    // `actionTriggered` covers every user-initiated scrollbar action
    // (arrow / track clicks, Page Up / Down, Home / End, drag, wheel
    // on the scrollbar) and fires synchronously before the resulting
    // `valueChanged`, so the flag we set here is still true when the
    // value-change handler runs.
    connect(vbar, &QAbstractSlider::actionTriggered, this, [this](int) { mNextValueChangeIsUser = true; });

    // Floating "jump to newest" pill, parented to the viewport so
    // it overlays rows without being clipped by the grid. The
    // view just forwards the click; `MainWindow` owns the
    // proxy-aware scroll and the Follow re-engage.
    mTailPill = new JumpToTailPill(viewport());
    mTailPill->SetArrowDirection(
        mTailEdge == TailEdge::Top ? JumpToTailPill::ArrowDirection::Up : JumpToTailPill::ArrowDirection::Down
    );
    connect(mTailPill, &QToolButton::clicked, this, [this]() { emit jumpToTailRequested(); });
    // Re-anchor on text-driven sizeHint changes (shrinking counts,
    // crossing the "999+" cap) -- `OnRowsInserted` only covers the
    // growth path.
    connect(mTailPill, &JumpToTailPill::contentSizeChanged, this, &LogTableView::PositionTailPill);

    // Event filter on the viewport drives `PositionTailPill` on
    // resize without subclassing the viewport.
    viewport()->installEventFilter(this);
}

void LogTableView::SetTailEdge(TailEdge edge)
{
    // Snapshot before overwriting so the pill-reset gate below can
    // see whether the orientation actually flipped.
    const bool edgeFlipped = (edge != mTailEdge);
    mTailEdge = edge;
    // Re-seed even when the edge didn't change: production callers
    // rely on this to absorb preceding programmatic scrollbar
    // mutations (see the header doc on `SetTailEdge`).
    mAtTailEdge = ComputeAtTailEdge(verticalScrollBar()->value());

    // Mirror onto the pill so the arrow points at the tail and the
    // anchor stays at the matching edge.
    if (mTailPill != nullptr)
    {
        mTailPill->SetArrowDirection(
            edge == TailEdge::Top ? JumpToTailPill::ArrowDirection::Up : JumpToTailPill::ArrowDirection::Down
        );
        PositionTailPill();
    }

    // Drop the pending tally when either the orientation flipped
    // (the count belongs to the old direction) or the re-seed
    // lands us at the new tail (user is current). Gating on
    // `mAtTailEdge` alone would strand a mid-scroll flip with a
    // stale count under the new arrow.
    if (edgeFlipped || mAtTailEdge)
    {
        ResetPendingNewRows();
    }
}

LogTableView::TailEdge LogTableView::GetTailEdge() const noexcept
{
    return mTailEdge;
}

void LogTableView::setModel(QAbstractItemModel *model)
{
    for (const auto &c : mModelConnections)
    {
        QObject::disconnect(c);
    }
    mModelConnections.clear();

    // Drop any anchor from the old model — it's owned by a model that
    // is about to disappear, and `QPersistentModelIndex` against the
    // new model would be a different beast.
    mPreservedAnchor = QPersistentModelIndex();
    mAnchorIsSaved = false;
    // The pending-new-rows tally belongs to the previous model.
    // Drop it before the new model attaches so it can't briefly
    // flash with a stale count if the new model has rows.
    ResetPendingNewRows();

    QTableView::setModel(model);

    if (model == nullptr)
    {
        return;
    }

    // Top-level row insertions and proxy-driven layout changes are
    // the two channels through which a batch reshuffles the table.
    // The save/restore pair preserves the user's reading position.
    mModelConnections.append(
        connect(model, &QAbstractItemModel::rowsAboutToBeInserted, this, &LogTableView::OnRowsAboutToBeInserted)
    );
    mModelConnections.append(connect(model, &QAbstractItemModel::rowsInserted, this, &LogTableView::OnRowsInserted));
    mModelConnections.append(
        connect(model, &QAbstractItemModel::layoutAboutToBeChanged, this, &LogTableView::OnLayoutAboutToBeChanged)
    );
    mModelConnections.append(connect(model, &QAbstractItemModel::layoutChanged, this, &LogTableView::OnLayoutChanged));
    // `modelReset` zeroes the rowcount, so any pending tally is
    // stale. Also covers the `clearAllFilters` -> proxy reset path,
    // which never emits `rowsRemoved`.
    mModelConnections.append(connect(model, &QAbstractItemModel::modelReset, this, &LogTableView::ResetPendingNewRows));
}

void LogTableView::keyPressEvent(QKeyEvent *event)
{
    if (event->matches(QKeySequence::Copy))
    {
        CopySelectedRowsToClipboard();
        return;
    }
    // Keyboard navigation may synchronously update the scrollbar; mark
    // the next `valueChanged` as user-initiated so it counts as a real
    // scroll away. Cleared on the way out so a non-scrolling key event
    // cannot leak the attribution onto a later programmatic change.
    mNextValueChangeIsUser = true;
    QTableView::keyPressEvent(event);
    mNextValueChangeIsUser = false;
}

void LogTableView::wheelEvent(QWheelEvent *event)
{
    mNextValueChangeIsUser = true;
    QTableView::wheelEvent(event);
    mNextValueChangeIsUser = false;
}

namespace
{
/// Spacing constants for the empty-state shortcuts card.
constexpr int CARD_PADDING_PX = 24;
constexpr int CARD_TITLE_GAP_PX = 16;
constexpr int CARD_GROUP_GAP_PX = 14;
constexpr int CARD_ROW_GAP_PX = 4;
constexpr int CARD_GUTTER_PX = 32;
constexpr int CARD_TITLE_POINT_BUMP = 4;
constexpr int CARD_GROUP_POINT_BUMP = 1;
constexpr qreal CARD_HEADING_OPACITY = 0.85;
constexpr qreal CARD_BODY_OPACITY = 0.65;

} // namespace

void LogTableView::paintEvent(QPaintEvent *event)
{
    QTableView::paintEvent(event);

    // Only draw on an empty grid; a populated session paints rows over us.
    if (model() != nullptr && model()->rowCount() > 0)
    {
        return;
    }

    const auto *mainWindow = qobject_cast<const QMainWindow *>(window());
    if (mainWindow == nullptr)
    {
        return;
    }

    const QList<shortcut_catalog::Group> groups = shortcut_catalog::Build(mainWindow);
    if (groups.isEmpty())
    {
        return;
    }

    QPainter painter(viewport());
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    const QPalette pal = palette();
    const QColor headingColor = pal.color(QPalette::WindowText);
    const QColor bodyColor = pal.color(QPalette::PlaceholderText);

    QFont titleFont = font();
    titleFont.setPointSize(titleFont.pointSize() + CARD_TITLE_POINT_BUMP);
    titleFont.setBold(true);

    QFont groupFont = font();
    groupFont.setPointSize(groupFont.pointSize() + CARD_GROUP_POINT_BUMP);
    groupFont.setBold(true);

    const QFont bodyFont = font();
    QFont shortcutFont = font();
    shortcutFont.setStyleHint(QFont::Monospace);

    const QFontMetrics titleFm(titleFont);
    const QFontMetrics groupFm(groupFont);
    const QFontMetrics bodyFm(bodyFont);
    const QFontMetrics shortcutFm(shortcutFont);

    const QString title = tr("Drop a JSON Lines log here, or use a shortcut below");

    // Measure widest label and shortcut so the two columns align across groups.
    int maxLabelWidth = 0;
    int maxShortcutWidth = 0;
    for (const auto &group : groups)
    {
        for (const auto &entry : group.entries)
        {
            maxLabelWidth = std::max(maxLabelWidth, bodyFm.horizontalAdvance(entry.text));
            maxShortcutWidth = std::max(maxShortcutWidth, shortcutFm.horizontalAdvance(entry.shortcut));
        }
    }

    const int rowHeight = std::max(bodyFm.height(), shortcutFm.height()) + CARD_ROW_GAP_PX;
    const int contentWidth = maxLabelWidth + CARD_GUTTER_PX + maxShortcutWidth;

    int contentHeight = titleFm.height() + CARD_TITLE_GAP_PX;
    for (qsizetype i = 0; i < groups.size(); ++i)
    {
        if (i > 0)
        {
            contentHeight += CARD_GROUP_GAP_PX;
        }
        contentHeight += groupFm.height() + CARD_ROW_GAP_PX;
        contentHeight += rowHeight * static_cast<int>(groups[i].entries.size());
    }

    const QRect viewportRect = viewport()->rect();
    const int cardWidth = std::min(viewportRect.width() - (2 * CARD_PADDING_PX), contentWidth);
    if (cardWidth <= 0)
    {
        return;
    }

    const int cardX = viewportRect.left() + ((viewportRect.width() - cardWidth) / 2);
    int y = viewportRect.top() + std::max(CARD_PADDING_PX, (viewportRect.height() - contentHeight) / 2);

    // Title
    painter.setOpacity(CARD_HEADING_OPACITY);
    painter.setPen(QPen(headingColor));
    painter.setFont(titleFont);
    painter.drawText(
        QRect(cardX, y, cardWidth, titleFm.height()),
        Qt::AlignHCenter | Qt::AlignTop,
        titleFm.elidedText(title, Qt::ElideRight, cardWidth)
    );
    y += titleFm.height() + CARD_TITLE_GAP_PX;

    const int labelColumnRight = cardX + maxLabelWidth;
    const int shortcutColumnLeft = labelColumnRight + CARD_GUTTER_PX;

    for (qsizetype i = 0; i < groups.size(); ++i)
    {
        if (i > 0)
        {
            y += CARD_GROUP_GAP_PX;
        }
        const auto &group = groups[i];

        painter.setOpacity(CARD_HEADING_OPACITY);
        painter.setPen(QPen(headingColor));
        painter.setFont(groupFont);
        painter.drawText(QRect(cardX, y, cardWidth, groupFm.height()), Qt::AlignLeft | Qt::AlignTop, group.title);
        y += groupFm.height() + CARD_ROW_GAP_PX;

        painter.setOpacity(CARD_BODY_OPACITY);
        painter.setPen(QPen(bodyColor));
        for (const auto &entry : group.entries)
        {
            painter.setFont(bodyFont);
            painter.drawText(
                QRect(cardX, y, maxLabelWidth, rowHeight),
                Qt::AlignLeft | Qt::AlignVCenter,
                bodyFm.elidedText(entry.text, Qt::ElideRight, maxLabelWidth)
            );
            painter.setFont(shortcutFont);
            painter.drawText(
                QRect(shortcutColumnLeft, y, maxShortcutWidth, rowHeight),
                Qt::AlignLeft | Qt::AlignVCenter,
                entry.shortcut
            );
            y += rowHeight;
        }
    }
}

bool LogTableView::ComputeAtTailEdge(int value) const
{
    const QScrollBar *bar = verticalScrollBar();
    if (mTailEdge == TailEdge::Bottom)
    {
        return value >= bar->maximum();
    }
    return value <= bar->minimum();
}

void LogTableView::OnVerticalScrollValueChanged(int value)
{
    // Always consume the flag: a stale `true` from an action that did
    // not actually move the slider must not leak onto a later
    // programmatic change.
    const bool wasUser = mNextValueChangeIsUser;
    mNextValueChangeIsUser = false;

    const bool atTailEdge = ComputeAtTailEdge(value);
    if (atTailEdge == mAtTailEdge)
    {
        return;
    }
    mAtTailEdge = atTailEdge;
    // Any tail-edge landing (user or programmatic) drops the
    // pending count so the pill disappears once the user is
    // caught up. The `wasUser` gate below still guards the
    // user-edge signals so Follow newest doesn't false-toggle.
    if (atTailEdge)
    {
        ResetPendingNewRows();
    }
    if (!wasUser)
    {
        // Programmatic / layout-driven change: refresh the flag (above)
        // so the *next* user transition edge-triggers, but stay silent.
        return;
    }
    if (atTailEdge)
    {
        emit userScrolledToTail();
    }
    else
    {
        emit userScrolledAwayFromTail();
    }
}

void LogTableView::OnVerticalScrollRangeChanged(int /*min*/, int /*max*/)
{
    // Refresh `mAtTailEdge` on range changes without emitting
    // user-scroll signals (a layout change is not a user scroll;
    // we must not toggle Follow newest as a side effect of rows
    // arriving).
    const int value = verticalScrollBar()->value();
    const bool atTailEdge = ComputeAtTailEdge(value);
    if (atTailEdge == mAtTailEdge)
    {
        return;
    }
    mAtTailEdge = atTailEdge;
    if (atTailEdge)
    {
        // Drifted into the tail because rows removed shrank the
        // range past `value`. Clear so the pill matches.
        ResetPendingNewRows();
    }
    // Transition off tail: don't reset, don't emit. The next
    // `rowsInserted` will increment from zero -- we accept losing
    // the in-flight batch's count since we cannot attribute its
    // rows after the fact.
}

void LogTableView::OnRowsAboutToBeInserted(const QModelIndex &parent, int /*first*/, int /*last*/)
{
    if (parent.isValid())
    {
        return;
    }
    SaveAnchorIfShouldPreserve();
}

void LogTableView::OnRowsInserted(const QModelIndex &parent, int first, int last)
{
    if (parent.isValid())
    {
        return;
    }
    RestoreAnchorIfSaved();

    // Add the batch to the pill counter when the user is scrolled
    // away and Follow newest is off. The slot is wired to the
    // outermost proxy, so `batch` already excludes filtered rows --
    // matching the user's mental model of "rows you missed since
    // you scrolled away".
    if (mAtTailEdge || mPendingNewRowsSuppressed || mTailPill == nullptr)
    {
        return;
    }
    const int batch = (last >= first) ? (last - first + 1) : 0;
    if (batch <= 0)
    {
        return;
    }
    mPendingNewRows += batch;
    mTailPill->SetCount(mPendingNewRows);
    // Direct reposition for the common "count grew" path; the
    // `contentSizeChanged` signal covers the same path too.
    PositionTailPill();
}

void LogTableView::OnLayoutAboutToBeChanged()
{
    SaveAnchorIfShouldPreserve();
}

void LogTableView::OnLayoutChanged()
{
    RestoreAnchorIfSaved();
}

void LogTableView::SaveAnchorIfShouldPreserve()
{
    mAnchorIsSaved = false;
    mPreservedAnchor = QPersistentModelIndex();

    // Only preserve in newest-first mode while the user is reading
    // older content. The default bottom-tail orientation keeps the
    // view stable on its own, and at the tail edge the user wants
    // either auto-scroll or the natural slide-in.
    if (mTailEdge != TailEdge::Top || mAtTailEdge)
    {
        return;
    }

    const QAbstractItemModel *m = model();
    if (m == nullptr || m->rowCount() == 0)
    {
        return;
    }

    // `(0, 1)` lands inside the first painted row even when its top
    // is flush with the viewport boundary.
    const QModelIndex top = indexAt(QPoint(0, 1));
    if (!top.isValid())
    {
        return;
    }

    const QRect rect = visualRect(top);
    mPreservedAnchor = QPersistentModelIndex(top);
    mPreservedAnchorOffsetPx = rect.top();
    mAnchorIsSaved = true;
}

void LogTableView::RestoreAnchorIfSaved()
{
    if (!mAnchorIsSaved)
    {
        return;
    }
    const QPersistentModelIndex anchor = mPreservedAnchor;
    const int targetOffsetPx = mPreservedAnchorOffsetPx;
    mAnchorIsSaved = false;
    mPreservedAnchor = QPersistentModelIndex();

    if (!anchor.isValid())
    {
        return;
    }

    const QRect rect = visualRect(anchor);
    const int delta = rect.top() - targetOffsetPx;
    if (delta == 0)
    {
        return;
    }

    QScrollBar *vbar = verticalScrollBar();
    // `mNextValueChangeIsUser` stays false here, so this `setValue`
    // is treated as programmatic by `OnVerticalScrollValueChanged`.
    vbar->setValue(vbar->value() + delta);
}

void LogTableView::PositionTailPill()
{
    if (mTailPill == nullptr)
    {
        return;
    }
    const QWidget *vp = viewport();
    if (vp == nullptr)
    {
        return;
    }
    // `adjustSize` first so we use the current sizeHint; an early
    // call (pre-`show`) would otherwise place the pill against a
    // 0x0 size and overshoot later.
    mTailPill->adjustSize();
    const QSize pillSize = mTailPill->size();
    const QRect vpRect = vp->rect();
    // Centre horizontally, clamped so a viewport narrower than the
    // pill keeps the click target fully visible (degraded but not
    // broken).
    const int rawX = vpRect.left() + ((vpRect.width() - pillSize.width()) / 2);
    const int maxX = vpRect.right() - pillSize.width() + 1;
    const int x = std::max(vpRect.left(), std::min(rawX, maxX));
    int y = 0;
    if (mTailEdge == TailEdge::Bottom)
    {
        y = vpRect.bottom() - pillSize.height() - PILL_VIEWPORT_MARGIN_PX;
    }
    else
    {
        y = vpRect.top() + PILL_VIEWPORT_MARGIN_PX;
    }
    mTailPill->move(x, y);
    mTailPill->raise();
}

void LogTableView::SetPendingNewRowsSuppressed(bool suppressed)
{
    if (suppressed == mPendingNewRowsSuppressed)
    {
        return;
    }
    mPendingNewRowsSuppressed = suppressed;
    // Engaging suppression also drops the pending tally:
    // toggling Follow newest back on counts as an acknowledgement
    // of whatever announcement was showing, so the pill should
    // clear immediately rather than freeze until the next
    // at-tail landing.
    if (suppressed)
    {
        ResetPendingNewRows();
    }
}

void LogTableView::AcknowledgePendingNewRows()
{
    ResetPendingNewRows();
}

void LogTableView::ResetPendingNewRows()
{
    // `isHidden()` instead of `!isVisible()`: under the offscreen
    // QPA used in CI tests, `isVisible()` is always false even for
    // a logically-shown pill, so it would mask the redundant-work
    // guard. `SetCount(0)` is idempotent regardless.
    if (mPendingNewRows == 0 && (mTailPill == nullptr || mTailPill->isHidden()))
    {
        return;
    }
    mPendingNewRows = 0;
    if (mTailPill != nullptr)
    {
        mTailPill->SetCount(0);
    }
}

bool LogTableView::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == viewport() && event != nullptr && event->type() == QEvent::Resize)
    {
        // Re-place the pill on every viewport resize. We fall
        // through to the base class so scroll-area maths and
        // header sizing still run.
        PositionTailPill();
    }
    return QTableView::eventFilter(watched, event);
}

void LogTableView::AttachOverviewRail(QWidget *rail)
{
    if (mOverviewRail == rail)
    {
        return;
    }
    // Detach the old rail (hide + drop from margin) without
    // deleting it — ownership stays with the caller. Reparent
    // to nullptr so a later attach against a different table
    // view is legal.
    if (mOverviewRail != nullptr)
    {
        mOverviewRail->hide();
        mOverviewRail->setParent(nullptr);
    }
    mOverviewRail = rail;
    if (rail == nullptr)
    {
        mReservedRightMargin = 0;
        // `setViewportMargins` is protected on QAbstractScrollArea;
        // we're a subclass so calling it is legal.
        setViewportMargins(0, 0, 0, 0);
        return;
    }
    rail->setParent(this);
    // Cache the width so `changeEvent` can refresh it on DPI /
    // style / font changes without querying the rail on every
    // geometry pass. Prefer `sizeHint` (the widget's own DPI-fluent
    // width) but fall back to `minimumSizeHint` and then the
    // current `width` so callers that ship a fixed-width rail
    // (`setFixedWidth`, `setMinimumWidth`) still reserve margin.
    mReservedRightMargin = ResolvedRailWidth(rail);
    setViewportMargins(0, 0, mReservedRightMargin, 0);
    rail->show();
    UpdateOverviewRailGeometry();
}

int LogTableView::ResolvedRailWidth(const QWidget *rail)
{
    if (rail == nullptr)
    {
        return 0;
    }
    const int hint = rail->sizeHint().width();
    if (hint > 0)
    {
        return hint;
    }
    const int minHint = rail->minimumSizeHint().width();
    if (minHint > 0)
    {
        return minHint;
    }
    return std::max(0, rail->width());
}

void LogTableView::resizeEvent(QResizeEvent *event)
{
    QTableView::resizeEvent(event);
    UpdateOverviewRailGeometry();
}

void LogTableView::changeEvent(QEvent *event)
{
    QTableView::changeEvent(event);
    if (event == nullptr || mOverviewRail == nullptr)
    {
        return;
    }
    // Refresh the reserved margin on events that can change the
    // rail's DPI-fluent width or its wash / colour palette.
    // `ScreenChangeInternal` is a private Qt enum; we accept the
    // one-time deprecation risk because it's the only signal that
    // reliably fires on a monitor DPR change across the Qt
    // versions the project supports (6.1+). Qt 6.6+ ships the
    // public `DevicePixelRatioChange` — once the minimum bumps
    // that far, replace this case with the newer event.
    switch (event->type())
    {
    case QEvent::StyleChange:
    case QEvent::PaletteChange:
    case QEvent::FontChange:
    case QEvent::ApplicationFontChange:
    case QEvent::ScreenChangeInternal:
    {
        const int freshWidth = ResolvedRailWidth(mOverviewRail);
        if (freshWidth != mReservedRightMargin)
        {
            mReservedRightMargin = freshWidth;
            setViewportMargins(0, 0, mReservedRightMargin, 0);
        }
        UpdateOverviewRailGeometry();
        break;
    }
    default:
        break;
    }
}

void LogTableView::UpdateOverviewRailGeometry()
{
    if (mOverviewRail == nullptr)
    {
        return;
    }
    const QWidget *vp = viewport();
    if (vp == nullptr)
    {
        return;
    }
    // Rail sits immediately to the right of the viewport,
    // spanning the viewport's Y range. Anchor to the viewport
    // (not the widget) so header / horizontal-scrollbar chrome
    // gets subtracted for free.
    const QRect vpGeom = vp->geometry();
    const int width = mReservedRightMargin;
    if (width <= 0)
    {
        return;
    }
    mOverviewRail->setGeometry(vpGeom.right() + 1, vpGeom.top(), width, vpGeom.height());
    mOverviewRail->raise();
}

void LogTableView::CopySelectedRowsToClipboard()
{
    const QModelIndexList selectedRows = this->selectionModel()->selectedRows();
    if (selectedRows.isEmpty())
    {
        return;
    }

    QString text;
    for (const QModelIndex &rowIndex : selectedRows)
    {
        const QVariant modelData = this->model()->data(rowIndex, LogModelItemDataRole::CopyLine);
        text += modelData.toString() + QLatin1Char('\n');
    }
    text.chop(1); // Drop the trailing newline

    QClipboard *clipboard = QApplication::clipboard();
    clipboard->setText(text);
}

void LogTableView::SetAnchorManager(AnchorManager *anchors) noexcept
{
    mAnchors = anchors;
}

std::vector<AnchorManager::Key> LogTableView::AnchorKeysForSelection() const
{
    std::vector<AnchorManager::Key> out;
    if (selectionModel() == nullptr || model() == nullptr)
    {
        return out;
    }
    const QModelIndexList selected = selectionModel()->selectedRows();
    if (selected.isEmpty())
    {
        return out;
    }

    // Walk the proxy chain down to the source `LogModel` so we can
    // ask for anchor keys.
    QAbstractItemModel *current = model();
    QModelIndexList resolvedIndices = selected;
    while (auto *proxy = qobject_cast<QAbstractProxyModel *>(current))
    {
        for (QModelIndex &idx : resolvedIndices)
        {
            idx = proxy->mapToSource(idx);
        }
        current = proxy->sourceModel();
        if (current == nullptr)
        {
            return out;
        }
    }
    const auto *logModel = qobject_cast<const LogModel *>(current);
    if (logModel == nullptr)
    {
        return out;
    }

    out.reserve(static_cast<std::size_t>(resolvedIndices.size()));
    for (const QModelIndex &sourceIndex : resolvedIndices)
    {
        if (!sourceIndex.isValid())
        {
            continue;
        }
        if (auto key = logModel->AnchorKeyForRow(sourceIndex.row()); key.has_value())
        {
            out.push_back(std::move(*key));
        }
    }
    return out;
}

void LogTableView::AnchorSelection(int colorIndex)
{
    if (mAnchors == nullptr || colorIndex < 0)
    {
        return;
    }
    const auto keys = AnchorKeysForSelection();
    if (keys.empty())
    {
        return;
    }
    // The bulk API routes signals based on actual mutation count.
    mAnchors->SetAnchors(keys, static_cast<uint8_t>(colorIndex));
}

void LogTableView::ClearAnchorOnSelection()
{
    if (mAnchors == nullptr)
    {
        return;
    }
    const auto keys = AnchorKeysForSelection();
    if (keys.empty())
    {
        return;
    }
    mAnchors->RemoveAnchors(keys);
}
