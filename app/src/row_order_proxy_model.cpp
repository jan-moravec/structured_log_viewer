#include "row_order_proxy_model.hpp"

#include <QList>

RowOrderProxyModel::RowOrderProxyModel(QObject *parent)
    : QAbstractProxyModel{parent}
{
}

void RowOrderProxyModel::DisconnectSourceConnections()
{
    QObject::disconnect(mRowsAboutToBeInsertedConn);
    QObject::disconnect(mRowsInsertedConn);
    QObject::disconnect(mRowsAboutToBeRemovedConn);
    QObject::disconnect(mRowsRemovedConn);
    QObject::disconnect(mColumnsAboutToBeInsertedConn);
    QObject::disconnect(mColumnsInsertedConn);
    QObject::disconnect(mColumnsAboutToBeRemovedConn);
    QObject::disconnect(mColumnsRemovedConn);
    QObject::disconnect(mColumnsAboutToBeMovedConn);
    QObject::disconnect(mColumnsMovedConn);
    QObject::disconnect(mDataChangedConn);
    QObject::disconnect(mHeaderDataChangedConn);
    QObject::disconnect(mModelAboutToBeResetConn);
    QObject::disconnect(mModelResetConn);
    QObject::disconnect(mLayoutAboutToBeChangedConn);
    QObject::disconnect(mLayoutChangedConn);
}

void RowOrderProxyModel::setSourceModel(QAbstractItemModel *sourceModel)
{
    if (this->sourceModel() == sourceModel)
    {
        return;
    }

    beginResetModel();
    DisconnectSourceConnections();
    // Clear any in-flight column-move bookkeeping: an outstanding
    // `begin` from the previous source would never see its `end`.
    mInSourceColumnMove = false;
    QAbstractProxyModel::setSourceModel(sourceModel);

    if (sourceModel != nullptr)
    {
        // Forward all structural changes. `LogModel`'s append + FIFO
        // evict is the only shape that hits production today, but
        // forwarding everything keeps the proxy correct under future
        // source-model evolution.
        mRowsAboutToBeInsertedConn = connect(
            sourceModel,
            &QAbstractItemModel::rowsAboutToBeInserted,
            this,
            [this](const QModelIndex &parent, int first, int last) {
                if (parent.isValid())
                {
                    return;
                }
                if (mReversed)
                {
                    // Source append at [first, last] maps to proxy
                    // [0, last - first]; existing proxy rows shift
                    // down by (last - first + 1). Qt remaps stored
                    // persistent indices via `beginInsertRows`.
                    beginInsertRows(QModelIndex(), 0, last - first);
                }
                else
                {
                    beginInsertRows(QModelIndex(), first, last);
                }
            }
        );

        mRowsInsertedConn =
            connect(sourceModel, &QAbstractItemModel::rowsInserted, this, [this](const QModelIndex &parent, int, int) {
                if (parent.isValid())
                {
                    return;
                }
                endInsertRows();
            });

        mRowsAboutToBeRemovedConn = connect(
            sourceModel,
            &QAbstractItemModel::rowsAboutToBeRemoved,
            this,
            [this](const QModelIndex &parent, int first, int last) {
                if (parent.isValid())
                {
                    return;
                }
                if (mReversed)
                {
                    // Use the pre-removal source count: by the time
                    // `rowsRemoved` fires the source's own `rowCount()`
                    // has already shrunk. FIFO eviction at source
                    // [0, last] maps to proxy [srcCount-1-last, srcCount-1].
                    const int preRemovalSourceRows = QAbstractProxyModel::sourceModel()->rowCount(QModelIndex());
                    beginRemoveRows(QModelIndex(), preRemovalSourceRows - 1 - last, preRemovalSourceRows - 1 - first);
                }
                else
                {
                    beginRemoveRows(QModelIndex(), first, last);
                }
            }
        );

        mRowsRemovedConn =
            connect(sourceModel, &QAbstractItemModel::rowsRemoved, this, [this](const QModelIndex &parent, int, int) {
                if (parent.isValid())
                {
                    return;
                }
                endRemoveRows();
            });

        mColumnsAboutToBeInsertedConn = connect(
            sourceModel,
            &QAbstractItemModel::columnsAboutToBeInserted,
            this,
            [this](const QModelIndex &parent, int first, int last) {
                if (parent.isValid())
                {
                    return;
                }
                beginInsertColumns(QModelIndex(), first, last);
            }
        );
        mColumnsInsertedConn = connect(
            sourceModel,
            &QAbstractItemModel::columnsInserted,
            this,
            [this](const QModelIndex &parent, int, int) {
                if (parent.isValid())
                {
                    return;
                }
                endInsertColumns();
            }
        );

        mColumnsAboutToBeRemovedConn = connect(
            sourceModel,
            &QAbstractItemModel::columnsAboutToBeRemoved,
            this,
            [this](const QModelIndex &parent, int first, int last) {
                if (parent.isValid())
                {
                    return;
                }
                beginRemoveColumns(QModelIndex(), first, last);
            }
        );
        mColumnsRemovedConn = connect(
            sourceModel,
            &QAbstractItemModel::columnsRemoved,
            this,
            [this](const QModelIndex &parent, int, int) {
                if (parent.isValid())
                {
                    return;
                }
                endRemoveColumns();
            }
        );

        mColumnsAboutToBeMovedConn = connect(
            sourceModel,
            &QAbstractItemModel::columnsAboutToBeMoved,
            this,
            [this](
                const QModelIndex &sourceParent,
                int sourceStart,
                int sourceEnd,
                const QModelIndex &destinationParent,
                int destinationColumn
            ) {
                // Skip hierarchical moves -- we don't model child
                // columns. Capture `beginMoveColumns`'s result so the
                // post-event slot only pairs with `endMoveColumns`
                // when Qt actually accepted the begin (an unpaired
                // end is a debug assertion).
                if (sourceParent.isValid() || destinationParent.isValid())
                {
                    mInSourceColumnMove = false;
                    return;
                }
                mInSourceColumnMove =
                    beginMoveColumns(QModelIndex(), sourceStart, sourceEnd, QModelIndex(), destinationColumn);
            }
        );
        mColumnsMovedConn = connect(
            sourceModel,
            &QAbstractItemModel::columnsMoved,
            this,
            [this](const QModelIndex &, int, int, const QModelIndex &, int) {
                if (mInSourceColumnMove)
                {
                    endMoveColumns();
                    mInSourceColumnMove = false;
                }
            }
        );

        // dataChanged + headerDataChanged: the row range is mirrored
        // in reversed mode; columns are untouched in both modes.
        mDataChangedConn =
            connect(sourceModel, &QAbstractItemModel::dataChanged, this, &RowOrderProxyModel::HandleSourceDataChanged);
        mHeaderDataChangedConn = connect(
            sourceModel,
            &QAbstractItemModel::headerDataChanged,
            this,
            [this](Qt::Orientation orientation, int first, int last) {
                emit headerDataChanged(orientation, first, last);
            }
        );

        // Reset / layoutChanged forwarding. `LogModel` doesn't emit
        // `layoutChanged` mid-stream, so we forward it as-is and let
        // Qt's bookkeeping remap the persistent indices.
        mModelAboutToBeResetConn =
            connect(sourceModel, &QAbstractItemModel::modelAboutToBeReset, this, [this]() { beginResetModel(); });
        mModelResetConn = connect(sourceModel, &QAbstractItemModel::modelReset, this, [this]() { endResetModel(); });

        mLayoutAboutToBeChangedConn = connect(
            sourceModel,
            &QAbstractItemModel::layoutAboutToBeChanged,
            this,
            [this](const QList<QPersistentModelIndex> &parents, QAbstractItemModel::LayoutChangeHint hint) {
                emit layoutAboutToBeChanged(parents, hint);
            }
        );
        mLayoutChangedConn = connect(
            sourceModel,
            &QAbstractItemModel::layoutChanged,
            this,
            [this](const QList<QPersistentModelIndex> &parents, QAbstractItemModel::LayoutChangeHint hint) {
                emit layoutChanged(parents, hint);
            }
        );
    }

    endResetModel();
}

QModelIndex RowOrderProxyModel::index(int row, int column, const QModelIndex &parent) const
{
    if (parent.isValid())
    {
        return {};
    }
    return createIndex(row, column);
}

QModelIndex RowOrderProxyModel::parent(const QModelIndex & /*child*/) const
{
    return {};
}

int RowOrderProxyModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid() || sourceModel() == nullptr)
    {
        return 0;
    }
    return sourceModel()->rowCount(QModelIndex());
}

int RowOrderProxyModel::columnCount(const QModelIndex &parent) const
{
    if (parent.isValid() || sourceModel() == nullptr)
    {
        return 0;
    }
    return sourceModel()->columnCount(QModelIndex());
}

QModelIndex RowOrderProxyModel::mapFromSource(const QModelIndex &sourceIndex) const
{
    if (!sourceIndex.isValid() || sourceModel() == nullptr)
    {
        return {};
    }
    if (sourceIndex.parent().isValid())
    {
        return {};
    }
    if (!mReversed)
    {
        return createIndex(sourceIndex.row(), sourceIndex.column());
    }
    const int rows = sourceModel()->rowCount(QModelIndex());
    if (rows == 0)
    {
        return {};
    }
    return createIndex(rows - 1 - sourceIndex.row(), sourceIndex.column());
}

QModelIndex RowOrderProxyModel::mapToSource(const QModelIndex &proxyIndex) const
{
    if (!proxyIndex.isValid() || sourceModel() == nullptr)
    {
        return {};
    }
    if (!mReversed)
    {
        return sourceModel()->index(proxyIndex.row(), proxyIndex.column(), QModelIndex());
    }
    const int rows = sourceModel()->rowCount(QModelIndex());
    if (rows == 0)
    {
        return {};
    }
    return sourceModel()->index(rows - 1 - proxyIndex.row(), proxyIndex.column(), QModelIndex());
}

void RowOrderProxyModel::HandleSourceDataChanged(
    const QModelIndex &topLeft, const QModelIndex &bottomRight, const QList<int> &roles
)
{
    if (!topLeft.isValid() || !bottomRight.isValid())
    {
        return;
    }
    if (!mReversed)
    {
        emit dataChanged(mapFromSource(topLeft), mapFromSource(bottomRight), roles);
        return;
    }
    // Reversed mode: source rect [tlRow..brRow] x [tlCol..brCol]
    // maps to proxy rect [rows-1-brRow..rows-1-tlRow] x [tlCol..brCol].
    // Build the proxy corners manually so the row range stays
    // anchored on the smaller-index corner.
    const int rows = sourceModel() == nullptr ? 0 : sourceModel()->rowCount(QModelIndex());
    if (rows == 0)
    {
        return;
    }
    const QModelIndex proxyTopLeft = createIndex(rows - 1 - bottomRight.row(), topLeft.column());
    const QModelIndex proxyBottomRight = createIndex(rows - 1 - topLeft.row(), bottomRight.column());
    emit dataChanged(proxyTopLeft, proxyBottomRight, roles);
}

void RowOrderProxyModel::SetReversed(bool reversed)
{
    if (mReversed == reversed)
    {
        return;
    }

    // O(persistent-indices) toggle: mirror each stored persistent
    // index around the row-count midpoint in lockstep with the
    // mapping flip. Even the empty-source case emits the layout pair
    // so observers re-bind; with no rows the remap is a no-op.
    emit layoutAboutToBeChanged();

    const QModelIndexList persistents = persistentIndexList();
    QModelIndexList remapped;
    remapped.reserve(persistents.size());
    const int rows = rowCount();
    for (const QModelIndex &idx : persistents)
    {
        if (!idx.isValid() || rows == 0)
        {
            remapped.append(idx);
            continue;
        }
        remapped.append(createIndex(rows - 1 - idx.row(), idx.column()));
    }

    mReversed = reversed;
    if (!persistents.isEmpty())
    {
        changePersistentIndexList(persistents, remapped);
    }

    emit layoutChanged();
}

bool RowOrderProxyModel::IsReversed() const noexcept
{
    return mReversed;
}
