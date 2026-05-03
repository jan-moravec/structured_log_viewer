#include "stream_order_proxy_model.hpp"

#include "log_model.hpp"

StreamOrderProxyModel::StreamOrderProxyModel(QObject *parent) : QSortFilterProxyModel{parent}
{
    // The reverse-order sort always compares `InsertionOrderRole`,
    // which `LogModel::data` returns as the bare source row index.
    // Setting the role once here means the user-clicked column sort on
    // the downstream `LogFilterModel` (which has its own sort role of
    // `LogModelItemDataRole::SortRole`) is completely independent of
    // ours — the two proxy layers never share sort state.
    setSortRole(LogModelItemDataRole::InsertionOrderRole);

    // Dynamic sort handles refiltering on data changes. The explicit
    // `invalidate()` from the source-`rowsInserted` hook below is
    // what guarantees newly-appended rows land at the visual top in
    // reversed mode — without it, only the snapshot present at the
    // time of `sort(...)` looks newest-first.
    setDynamicSortFilter(true);
}

void StreamOrderProxyModel::setSourceModel(QAbstractItemModel *sourceModel)
{
    // Drop only the connections we own; `QSortFilterProxyModel`
    // manages its own source-model plumbing internally and must not
    // be touched here.
    QObject::disconnect(mRowsInsertedConn);
    QObject::disconnect(mRowsRemovedConn);

    QSortFilterProxyModel::setSourceModel(sourceModel);

    if (sourceModel == nullptr)
    {
        return;
    }

    // Slots fire in connection order, so the base class'
    // `_q_sourceRowsInserted` (registered earlier inside
    // `QSortFilterProxyModel::setSourceModel`) has already updated
    // the proxy mapping by the time our handler runs — `invalidate()`
    // is therefore a safe direct call.
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

    // `sort(0, ...)` with the `InsertionOrderRole` set above performs
    // a row-index sort — the column argument is incidental, since the
    // role's value is identical for every column on a given row.
    // `sort(-1)` clears the proxy's sort state and falls back to the
    // identity mapping.
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
