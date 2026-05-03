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

LogTableView::LogTableView(QWidget *parent) : QTableView(parent)
{
    setAcceptDrops(true);

    QScrollBar *vbar = verticalScrollBar();

    // Edge-triggered emission of `userScrolledAwayFromTail` /
    // `userScrolledToTail` so the `MainWindow` only flips the
    // **Follow newest** toggle on transition. The handler
    // is gated on `mNextValueChangeIsUser` so non-user value changes
    // (programmatic `scrollTo`, value clamping driven by
    // `endInsertRows`, hover/repaint-induced internal scroll updates,
    // viewport resizes, our own scroll-anchor preservation) never
    // disengage Follow newest on their own.
    connect(vbar, &QAbstractSlider::valueChanged, this, &LogTableView::OnVerticalScrollValueChanged);

    // `actionTriggered` covers every user-initiated scrollbar action:
    // arrow / track clicks, Page Up / Down, Home / End, drag start,
    // wheel-on-the-scrollbar (which flows through
    // `QAbstractSlider::wheelEvent` → `triggerAction(SliderMove)`).
    // It fires *synchronously before* the slider value is actually
    // updated, so the `mNextValueChangeIsUser` flag we set here is
    // still true when the resulting `valueChanged` fires inside the
    // same call stack.
    //
    // The previous `installEventFilter` approach was broken: an event
    // filter runs *before* the watched widget's own event handler, so
    // setting `mUserInteractionDepth` around the filter call left the
    // counter back at zero by the time the scrollbar processed the
    // mouse / wheel event and emitted `valueChanged` — direct
    // scrollbar interactions therefore failed to disengage Follow
    // newest, exactly the bug the user reported.
    connect(vbar, &QAbstractSlider::actionTriggered, this, [this](int) { mNextValueChangeIsUser = true; });
}

void LogTableView::SetTailEdge(TailEdge edge)
{
    mTailEdge = edge;
    // Re-seed `mAtTailEdge` against the current scroll position so the
    // next genuine user transition still edge-triggers correctly.
    // Without this re-seed a flip from `Bottom` → `Top` (or vice
    // versa) while the view is at one extreme would let the next
    // `valueChanged` mistake the *initial* read of the new edge for a
    // real transition.
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

    // Top-level row insertions and proxy-driven layout changes are the
    // two channels through which a streaming batch reorganises the
    // table in newest-first mode. We snapshot a scroll anchor *before*
    // the change, then restore it *after*, so the user's reading
    // position survives the structural update intact (chat-app
    // pattern). The hooks no-op when the user is at the tail edge or
    // when the orientation is the default `TailEdge::Bottom`.
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
    // Keyboard navigation (arrow keys, Page Up/Down, Home/End, etc.)
    // may synchronously update the vertical scrollbar from inside Qt's
    // base implementation. Mark the next `valueChanged` as
    // user-initiated so a resulting "no longer at tail" transition is
    // treated as a real scroll away. The flag is cleared
    // unconditionally on the way out so a key event that doesn't
    // happen to scroll cannot leak its "user" attribution onto the
    // *next* (potentially programmatic) value change.
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
    // Always consume the user-input flag so a stale "true" from an
    // action that didn't actually move the slider (e.g. clicking the
    // up arrow when already at minimum) cannot leak onto a later
    // programmatic `setValue`.
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
        // Programmatic / layout-driven change. Refresh `mAtTailEdge`
        // (already done above) so the *next* genuine user transition
        // still edge-triggers, but never fire the toggle-flipping
        // signals from a non-user origin.
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

    // Preserve only in newest-first orientation when the user is
    // reading older content. In the default bottom-tail orientation
    // Qt's append-at-bottom semantics already keep the user's view
    // stable (the scrollbar maximum grows but the pixel offset of the
    // visible content does not). At the tail edge the user wants
    // either the auto-scroll (Follow newest on) or the natural
    // "newest line slides in at the very edge" behaviour, so the
    // anchor is unnecessary.
    if (mTailEdge != TailEdge::Top || mAtTailEdge)
    {
        return;
    }

    QAbstractItemModel *m = model();
    if (m == nullptr || m->rowCount() == 0)
    {
        return;
    }

    // Pick the topmost visible row as the anchor. `(0, 1)` lands
    // inside the first painted row even when the row's top is flush
    // with the viewport boundary.
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
    // `setValue` is treated as programmatic by
    // `OnVerticalScrollValueChanged` because `mNextValueChangeIsUser`
    // is `false` here (we never set it from this path). Clamping to
    // the slider's range is handled by `QAbstractSlider::setValue`.
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
