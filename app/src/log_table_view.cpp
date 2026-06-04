#include "log_table_view.hpp"

#include "anchor_manager.hpp"
#include "shortcut_catalog.hpp"

#include <log_model.hpp>

#include <QAbstractItemModel>
#include <QAbstractProxyModel>
#include <QApplication>
#include <QClipboard>
#include <QFont>
#include <QFontMetrics>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QMainWindow>
#include <QPaintEvent>
#include <QPainter>
#include <QPalette>
#include <QPen>
#include <QPoint>
#include <QRect>
#include <QScrollBar>
#include <QString>
#include <QWheelEvent>

#include <algorithm>
#include <utility>

LogTableView::LogTableView(QWidget *parent)
    : QTableView(parent)
{
    setAcceptDrops(true);

    const QScrollBar *vbar = verticalScrollBar();

    // Edge-triggered scroll detection gated on `mNextValueChangeIsUser`
    // so non-user changes (programmatic `scrollTo`, `endInsertRows`
    // clamping, our anchor restore) cannot disengage Follow newest.
    connect(vbar, &QAbstractSlider::valueChanged, this, &LogTableView::OnVerticalScrollValueChanged);

    // `actionTriggered` covers every user-initiated scrollbar action
    // (arrow / track clicks, Page Up / Down, Home / End, drag, wheel
    // on the scrollbar) and fires synchronously before the resulting
    // `valueChanged`, so the flag we set here is still true when the
    // value-change handler runs.
    connect(vbar, &QAbstractSlider::actionTriggered, this, [this](int) { mNextValueChangeIsUser = true; });
}

void LogTableView::SetTailEdge(TailEdge edge)
{
    mTailEdge = edge;
    // Re-seed against the current scroll position so a flip from one
    // edge to the other does not register as a user transition.
    mAtTailEdge = ComputeAtTailEdge(verticalScrollBar()->value());
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

    const QList<ShortcutCatalog::Group> groups = ShortcutCatalog::Build(mainWindow);
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

void LogTableView::OnRowsAboutToBeInserted(const QModelIndex &parent, int /*first*/, int /*last*/)
{
    if (parent.isValid())
    {
        return;
    }
    SaveAnchorIfShouldPreserve();
}

void LogTableView::OnRowsInserted(const QModelIndex &parent, int /*first*/, int /*last*/)
{
    if (parent.isValid())
    {
        return;
    }
    RestoreAnchorIfSaved();
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
