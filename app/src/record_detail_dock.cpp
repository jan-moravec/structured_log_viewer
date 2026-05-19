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
    : QDockWidget(QObject::tr("Record Details"), parent), mModel(model)
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

    connect(mWidget, &RecordDetailWidget::openInNewWindowRequested, this, &RecordDetailDock::OnOpenInNewWindowRequested);

    // Refresh after FIFO eviction so the summary's "Row N" label
    // tracks the row's new position, and so the placeholder kicks
    // in when the pinned row was evicted itself
    // (`QPersistentModelIndex::isValid()` flips to false). Qt fires
    // `rowsRemoved` on the source model after the removal commits.
    if (mModel != nullptr)
    {
        connect(mModel, &QAbstractItemModel::rowsRemoved, this, [this](const QModelIndex &, int, int) {
            if (mCurrentSourceIndex.isValid())
            {
                RefreshFromModel();
            }
            else
            {
                // Pinned row was inside the evicted range: show the
                // placeholder explaining that the record is gone.
                Clear();
            }
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
                const int pinnedRow = mCurrentSourceIndex.row();
                if (pinnedRow >= topLeft.row() && pinnedRow <= bottomRight.row())
                {
                    RefreshFromModel();
                }
            }
        );
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
    RefreshFromModel();
}

void RecordDetailDock::Clear()
{
    mCurrentSourceIndex = QPersistentModelIndex();
    RecordDetailContent placeholder;
    placeholder.valid = false;
    placeholder.placeholderText = DefaultRecordDetailPlaceholder();
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
}

void RecordDetailDock::OnOpenInNewWindowRequested()
{
    // Relay the live row through the persistent index so the owner
    // builds a snapshot of the actual record (post-eviction shifts
    // and all) rather than a possibly-stale integer.
    emit openInNewWindowRequested(CurrentSourceRow());
}
