#include "record_detail_dock.hpp"

#include "log_model.hpp"
#include "record_detail_widget.hpp"

#include <QAbstractItemModel>
#include <QList>
#include <QModelIndex>
#include <QObject>

namespace
{
/// Floor for the dock's width. Picked so the field/value table
/// stays readable without forcing the central log view too narrow
/// on small displays. `addDockWidget` still picks a larger initial
/// size if the dock's `sizeHint()` is bigger.
constexpr int DOCK_MIN_WIDTH = 280;
} // namespace

RecordDetailDock::RecordDetailDock(LogModel *model, QWidget *parent)
    : QDockWidget(tr("Record Details"), parent), mModel(model)
{
    setObjectName(QStringLiteral("recordDetailDock"));
    // Allow docking on either side, float as a top-level window, and
    // close via the title bar. The View menu's `toggleViewAction()`
    // mirrors the open/closed state.
    setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea | Qt::BottomDockWidgetArea);
    setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);

    mWidget = new RecordDetailWidget(this);
    setWidget(mWidget);
    setMinimumWidth(DOCK_MIN_WIDTH);

    // Floor for side-dock width only -- when the user drops the
    // dock at the bottom the field/value table can wrap freely, so
    // a narrow horizontal slot (short history under a wide log
    // table) is the more useful default. Toggle the min-width to
    // 0 on bottom-area transitions.
    connect(this, &QDockWidget::dockLocationChanged, this, [this](Qt::DockWidgetArea area) {
        setMinimumWidth(area == Qt::BottomDockWidgetArea ? 0 : DOCK_MIN_WIDTH);
    });

    connect(mWidget, &RecordDetailWidget::openInNewWindowRequested, this, &RecordDetailDock::OnOpenInNewWindowRequested);

    // Track our own "is the user actually seeing this pane" state.
    // `isHidden()` alone misses tabified-dock-area cases where the
    // dock's tab is buried behind another dock's tab: the explicit
    // hide flag stays false but `visibilityChanged(false)` fires.
    // We use both checks together through `IsVisibleForRefresh()`.
    // When the user surfaces the dock again, refresh once so the
    // content reflects any model mutations we deliberately ignored
    // while invisible (FIFO eviction shifts, column moves, ...).
    connect(this, &QDockWidget::visibilityChanged, this, [this](bool visible) {
        const bool wasVisible = mPerceivedVisible;
        mPerceivedVisible = visible;
        if (visible && !wasVisible && mCurrentSourceIndex.isValid())
        {
            RefreshFromModel();
        }
    });

    // Refresh after FIFO eviction so the summary's "Row N" label
    // tracks the row's new position, and so the placeholder kicks
    // in when the pinned row was evicted itself
    // (`QPersistentModelIndex::isValid()` flips to false). Qt fires
    // `rowsRemoved` on the source model after the removal commits.
    if (mModel != nullptr)
    {
        // `modelReset` (file open / replace, `LogModel::Reset`) blows
        // away every row without firing `rowsRemoved`, so the
        // persistent index is left dangling and our placeholder text
        // would stay stale. Listening directly inside the dock keeps
        // it self-contained -- the previous design relied on
        // `MainWindow` calling `Clear()` from its own `modelReset`
        // lambda, which silently regressed any future reuse of the
        // dock outside `MainWindow` (e.g. tests, future tools).
        connect(mModel, &QAbstractItemModel::modelReset, this, &RecordDetailDock::Clear);
        connect(mModel, &QAbstractItemModel::rowsRemoved, this, [this](const QModelIndex &, int, int) {
            if (mCurrentSourceIndex.isValid())
            {
                // Pinned row survives -- only its index shifted. Skip
                // the rebuild when the user isn't looking; the
                // `visibilityChanged(true)` handler above refreshes
                // once on re-surface, and `MainWindow::
                // UpdateRecordDetailsFromSelection` will re-pin from
                // the table's current selection too.
                if (!IsVisibleForRefresh())
                {
                    return;
                }
                RefreshFromModel();
            }
            else if (mEverPinned)
            {
                // The persistent index went invalid this tick -- the
                // pinned record was inside the evicted range. Always
                // swap to the dedicated placeholder (cheap text
                // update, no field-table rebuild) so the next show
                // surfaces "your record is gone" rather than the
                // default "select a row" prompt. `mEverPinned` stays
                // true so subsequent removals are an idempotent no-op
                // until the next `Clear` / `ShowSourceRow`.
                ShowEvictedPlaceholder();
            }
            // else: no pin to begin with -- a fresh dock that never
            // had a row selected. Streaming eviction shouldn't pay
            // for a placeholder rebuild that has no visible effect.
        });
        // Refresh when the pinned row's data changes underneath us.
        // Streaming back-fill, an out-of-band column edit, or an
        // enum-column promotion all emit `dataChanged` covering the
        // affected row range; without this connection the pane would
        // keep showing pre-change values until the user re-selected
        // the row.
        connect(
            mModel,
            &QAbstractItemModel::dataChanged,
            this,
            [this](const QModelIndex &topLeft, const QModelIndex &bottomRight, const QList<int> & /*roles*/) {
                if (!mCurrentSourceIndex.isValid())
                {
                    return;
                }
                // Same visibility gate as the `rowsRemoved` handler:
                // an invisible dock has nothing to display, and we
                // refresh on next show via the visibility hook plus
                // the selection re-pin.
                if (!IsVisibleForRefresh())
                {
                    return;
                }
                const int pinnedRow = mCurrentSourceIndex.row();
                if (pinnedRow >= topLeft.row() && pinnedRow <= bottomRight.row())
                {
                    RefreshFromModel();
                }
            }
        );
        // Column-order events: a header drag and the streaming
        // `Time` column bubble both emit `columnsMoved`, and the
        // first batch of a new key emits `columnsInserted`. Both
        // change the order our `Field / Value` table renders, so
        // an unrefreshed pin would show stale column order until
        // the user re-selected the row. The pinned row itself is
        // unaffected (the persistent index pins column 0, which
        // `LogModel` never removes); we just need to rebuild the
        // field list. `LogModel` doesn't emit `columnsRemoved`
        // today, so we skip that wire to avoid noise.
        auto columnsLayoutChanged = [this]() {
            if (!IsVisibleForRefresh() || !mCurrentSourceIndex.isValid())
            {
                return;
            }
            RefreshFromModel();
        };
        connect(mModel, &QAbstractItemModel::columnsMoved, this, columnsLayoutChanged);
        connect(mModel, &QAbstractItemModel::columnsInserted, this, columnsLayoutChanged);
    }

    Clear();
}

int RecordDetailDock::CurrentSourceRow() const noexcept
{
    return mCurrentSourceIndex.isValid() ? mCurrentSourceIndex.row() : -1;
}

void RecordDetailDock::ShowSourceRow(int sourceRow)
{
    if (!mModel)
    {
        Clear();
        return;
    }
    if (sourceRow < 0 || sourceRow >= mModel->rowCount())
    {
        Clear();
        return;
    }
    mCurrentSourceIndex = QPersistentModelIndex(mModel->index(sourceRow, 0));
    mEverPinned = true;
    RefreshFromModel();
}

void RecordDetailDock::Clear()
{
    mCurrentSourceIndex = QPersistentModelIndex();
    mEverPinned = false;
    RecordDetailContent placeholder;
    placeholder.valid = false;
    placeholder.placeholderText = DefaultRecordDetailPlaceholder();
    mWidget->SetContent(placeholder);
}

void RecordDetailDock::ShowEvictedPlaceholder()
{
    // The persistent index is already invalid by the time we get
    // here (Qt invalidated it during `rowsRemoved`); explicitly
    // drop it anyway so future `CurrentSourceRow()` reads are
    // deterministic without depending on Qt's invariant.
    mCurrentSourceIndex = QPersistentModelIndex();
    RecordDetailContent placeholder;
    placeholder.valid = false;
    placeholder.placeholderText = EvictedRecordPlaceholder();
    mWidget->SetContent(placeholder);
}

void RecordDetailDock::RefreshFromModel()
{
    if (!mModel || !mCurrentSourceIndex.isValid())
    {
        Clear();
        return;
    }
    mWidget->SetContent(BuildRecordDetailContent(*mModel, mCurrentSourceIndex.row()));
#ifdef LOGAPP_BUILD_TESTING
    ++mRefreshCount;
#endif
}

bool RecordDetailDock::IsVisibleForRefresh() const noexcept
{
    return !isHidden() && mPerceivedVisible;
}

void RecordDetailDock::OnOpenInNewWindowRequested()
{
    // Relay the live row through the persistent index so the owner
    // builds a snapshot of the actual record (post-eviction shifts
    // and all) rather than a possibly-stale integer.
    emit openInNewWindowRequested(CurrentSourceRow());
}
