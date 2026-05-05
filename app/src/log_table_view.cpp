#include "log_table_view.hpp"

#include <log_model.hpp>

#include <QAbstractItemModel>
#include <QApplication>
#include <QClipboard>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QPoint>
#include <QRect>
#include <QScrollBar>
#include <QWheelEvent>

LogTableView::LogTableView(QWidget *parent)
    : QTableView(parent)
{
    setAcceptDrops(true);

    QScrollBar *vbar = verticalScrollBar();

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

bool LogTableView::ComputeAtTailEdge(int value) const
{
    QScrollBar *bar = verticalScrollBar();
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

    QAbstractItemModel *m = model();
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
