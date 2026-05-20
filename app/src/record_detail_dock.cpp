#include "record_detail_dock.hpp"

#include "log_model.hpp"
#include "record_detail_widget.hpp"

#include <QAbstractItemModel>
#include <QList>
#include <QModelIndex>
#include <QObject>

namespace
{
/// Minimum width for side-docked layout. Keeps the field/value table
/// readable without squeezing the central log view too much.
constexpr int DOCK_MIN_WIDTH = 280;
} // namespace

RecordDetailDock::RecordDetailDock(LogModel *model, QWidget *parent)
    : QDockWidget(tr("Record Details"), parent), mModel(model)
{
    setObjectName(QStringLiteral("recordDetailDock"));
    setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea | Qt::BottomDockWidgetArea);
    setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);

    mWidget = new RecordDetailWidget(this);
    setWidget(mWidget);
    setMinimumWidth(DOCK_MIN_WIDTH);

    // Side-dock width floor only. When docked at the bottom the
    // field/value table can wrap freely, so a narrow horizontal slot
    // is more useful than a hard minimum.
    connect(this, &QDockWidget::dockLocationChanged, this, [this](Qt::DockWidgetArea area) {
        setMinimumWidth(area == Qt::BottomDockWidgetArea ? 0 : DOCK_MIN_WIDTH);
    });

    connect(
        mWidget, &RecordDetailWidget::openInNewWindowRequested, this, &RecordDetailDock::OnOpenInNewWindowRequested
    );

    // Track "is the user actually seeing this pane". `isHidden()`
    // alone misses tabified-dock cases where our tab is buried but
    // the explicit-hide flag is still false. On resume, refresh once
    // to catch up on changes we ignored while invisible.
    connect(this, &QDockWidget::visibilityChanged, this, [this](bool visible) {
        const bool wasVisible = mPerceivedVisible;
        mPerceivedVisible = visible;
        if (visible && !wasVisible && mCurrentSourceIndex.isValid())
        {
            RefreshFromModel();
        }
    });

    if (mModel != nullptr)
    {
        // `modelReset` (file open, `LogModel::Reset`) drops every row
        // without firing `rowsRemoved`. Listening here keeps the dock
        // self-contained -- callers don't have to wire it.
        connect(mModel, &QAbstractItemModel::modelReset, this, &RecordDetailDock::Clear);
        connect(mModel, &QAbstractItemModel::rowsRemoved, this, [this](const QModelIndex &, int, int) {
            if (mCurrentSourceIndex.isValid())
            {
                // Pinned row survives, only its index shifted; refresh
                // so the summary's row number is current.
                if (!IsVisibleForRefresh())
                {
                    return;
                }
                RefreshFromModel();
            }
            else if (mEverPinned)
            {
                // Pinned row was just evicted. Always swap to the
                // dedicated placeholder (cheap; no field rebuild) so
                // the next show surfaces "record is gone" instead of
                // the default "select a row" prompt.
                ShowEvictedPlaceholder();
            }
            // else: never pinned -- nothing to invalidate.
        });
        // Pinned-row edits (back-fill, header rename, enum promotion)
        // emit `dataChanged` covering the row. Without this the pane
        // would show stale values until the user re-selects the row.
        connect(
            mModel,
            &QAbstractItemModel::dataChanged,
            this,
            [this](const QModelIndex &topLeft, const QModelIndex &bottomRight, const QList<int> & /*roles*/) {
                if (!mCurrentSourceIndex.isValid())
                {
                    return;
                }
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
        // Column reorders (header drag, streaming `Time` bubble) and
        // new-key inserts change the order the Field/Value table
        // renders, so rebuild the field list. The pinned row itself
        // is unaffected -- column 0 is never removed.
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
    if (!mCurrentSourceIndex.isValid())
    {
        // Defensive: only happens if a future `LogModel` layout
        // produces zero columns with positive `rowCount`. Avoids
        // latching `mEverPinned` on a useless pin.
        Clear();
        return;
    }
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
    // Qt invalidated the persistent index during `rowsRemoved`;
    // reset it explicitly so `CurrentSourceRow()` is deterministic.
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
    // Read the row through the persistent index so eviction shifts
    // are reflected and the snapshot points at the actual record.
    emit openInNewWindowRequested(CurrentSourceRow());
}
