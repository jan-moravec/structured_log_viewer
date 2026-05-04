#include "stream_order_proxy_model.hpp"

#include "log_model.hpp"

StreamOrderProxyModel::StreamOrderProxyModel(QObject *parent)
    : QSortFilterProxyModel{parent}
{
    // Compare on the bare source row index; the downstream
    // `LogFilterModel` sorts on `SortRole`, so the two proxies'
    // sort states never overlap.
    setSortRole(LogModelItemDataRole::InsertionOrderRole);

    // Dynamic sort + the explicit `invalidate()` below ensures
    // newly-appended rows always land at the visual top in reversed
    // mode (without it, only the snapshot at `sort()`-time is sorted).
    setDynamicSortFilter(true);
}

void StreamOrderProxyModel::setSourceModel(QAbstractItemModel *sourceModel)
{
    // Drop only the connections we own; the base class manages its
    // own source-model plumbing.
    QObject::disconnect(mRowsInsertedConn);
    QObject::disconnect(mRowsRemovedConn);

    QSortFilterProxyModel::setSourceModel(sourceModel);

    if (sourceModel == nullptr)
    {
        return;
    }

    // Slots fire in connection order, so by the time our handler runs
    // the base class has already updated its mapping; calling
    // `invalidate()` here is safe.
    auto resortIfReversed = [this](const QModelIndex &parent, int, int) {
        if (mReversed && !parent.isValid())
        {
            invalidate();
        }
    };
    mRowsInsertedConn = connect(sourceModel, &QAbstractItemModel::rowsInserted, this, resortIfReversed);
    mRowsRemovedConn = connect(sourceModel, &QAbstractItemModel::rowsRemoved, this, resortIfReversed);
}

void StreamOrderProxyModel::SetReversed(bool reversed)
{
    if (mReversed == reversed)
    {
        return;
    }
    mReversed = reversed;

    // The column argument to `sort()` is incidental because
    // `InsertionOrderRole` is identical across all columns of a row.
    // `sort(-1)` falls back to the identity mapping.
    if (mReversed)
    {
        sort(0, Qt::DescendingOrder);
    }
    else
    {
        sort(-1);
    }
}

bool StreamOrderProxyModel::IsReversed() const noexcept
{
    return mReversed;
}
