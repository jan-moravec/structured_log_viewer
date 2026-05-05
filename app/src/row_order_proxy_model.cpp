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
    QAbstractProxyModel::setSourceModel(sourceModel);

    if (sourceModel != nullptr)
    {
        // Structural-change forwarding: rows and columns. The append-
        // and-FIFO-evict shape of `LogModel` is the only structural
        // change that lands here in production, but we forward every
        // structural signal so the proxy stays correct under any
        // future source-model evolution.
        mRowsAboutToBeInsertedConn = connect(
            sourceModel, &QAbstractItemModel::rowsAboutToBeInserted, this,
            [this](const QModelIndex &parent, int first, int last) {
                if (parent.isValid())
                {
                    return;
                }
                if (mReversed)
                {
                    // Append at source [first, last] lands at proxy
                    // [0, last - first]. Existing proxy rows shift
                    // down by (last - first + 1); Qt remaps stored
                    // persistent indices accordingly via beginInsertRows.
                    beginInsertRows(QModelIndex(), 0, last - first);
                }
                else
                {
                    beginInsertRows(QModelIndex(), first, last);
                }
            }
        );

        mRowsInsertedConn = connect(
            sourceModel, &QAbstractItemModel::rowsInserted, this,
            [this](const QModelIndex &parent, int, int) {
                if (parent.isValid())
                {
                    return;
                }
                endInsertRows();
            }
        );

        mRowsAboutToBeRemovedConn = connect(
            sourceModel, &QAbstractItemModel::rowsAboutToBeRemoved, this,
            [this](const QModelIndex &parent, int first, int last) {
                if (parent.isValid())
                {
                    return;
                }
                if (mReversed)
                {
                    // Capture the pre-removal source row count: the
                    // proxy-side range we need to report depends on
                    // it, and the source's `rowCount()` will already
                    // have been updated by the time `rowsRemoved`
                    // fires. FIFO eviction at source [0, last] maps
                    // to proxy [srcCount-1-last, srcCount-1].
                    mPreRemovalSourceRows = QAbstractProxyModel::sourceModel()->rowCount(QModelIndex());
                    beginRemoveRows(
                        QModelIndex(), mPreRemovalSourceRows - 1 - last, mPreRemovalSourceRows - 1 - first
                    );
                }
                else
                {
                    beginRemoveRows(QModelIndex(), first, last);
                }
            }
        );

        mRowsRemovedConn = connect(
            sourceModel, &QAbstractItemModel::rowsRemoved, this,
            [this](const QModelIndex &parent, int, int) {
                if (parent.isValid())
                {
                    return;
                }
                endRemoveRows();
            }
        );

        mColumnsAboutToBeInsertedConn = connect(
            sourceModel, &QAbstractItemModel::columnsAboutToBeInserted, this,
            [this](const QModelIndex &parent, int first, int last) {
                if (parent.isValid())
                {
                    return;
                }
                beginInsertColumns(QModelIndex(), first, last);
            }
        );
        mColumnsInsertedConn = connect(
            sourceModel, &QAbstractItemModel::columnsInserted, this,
            [this](const QModelIndex &parent, int, int) {
                if (parent.isValid())
                {
                    return;
                }
                endInsertColumns();
            }
        );

        mColumnsAboutToBeRemovedConn = connect(
            sourceModel, &QAbstractItemModel::columnsAboutToBeRemoved, this,
            [this](const QModelIndex &parent, int first, int last) {
                if (parent.isValid())
                {
                    return;
                }
                beginRemoveColumns(QModelIndex(), first, last);
            }
        );
        mColumnsRemovedConn = connect(
            sourceModel, &QAbstractItemModel::columnsRemoved, this,
            [this](const QModelIndex &parent, int, int) {
                if (parent.isValid())
                {
                    return;
                }
                endRemoveColumns();
            }
        );

        mColumnsAboutToBeMovedConn = connect(
            sourceModel, &QAbstractItemModel::columnsAboutToBeMoved, this,
            [this](
                const QModelIndex &sourceParent,
                int sourceStart,
                int sourceEnd,
                const QModelIndex &destinationParent,
                int destinationColumn
            ) {
                if (sourceParent.isValid() || destinationParent.isValid())
                {
                    return;
                }
                beginMoveColumns(QModelIndex(), sourceStart, sourceEnd, QModelIndex(), destinationColumn);
            }
        );
        mColumnsMovedConn = connect(
            sourceModel, &QAbstractItemModel::columnsMoved, this,
            [this](const QModelIndex &, int, int, const QModelIndex &, int) { endMoveColumns(); }
        );

        // dataChanged + headerDataChanged: the row range is mirrored
        // in reversed mode; columns are unchanged in both modes.
        mDataChangedConn = connect(
            sourceModel, &QAbstractItemModel::dataChanged, this, &RowOrderProxyModel::HandleSourceDataChanged
        );
        mHeaderDataChangedConn = connect(
            sourceModel, &QAbstractItemModel::headerDataChanged, this,
            [this](Qt::Orientation orientation, int first, int last) { emit headerDataChanged(orientation, first, last); }
        );

        // Reset / layoutChanged forwarding. `layoutChanged` may
        // invalidate the row mapping (the proxy's persistent indices
        // need re-mapping), but `LogModel` does not emit it
        // mid-stream so we forward it as-is and Qt's bookkeeping
        // takes care of the persistents.
        mModelAboutToBeResetConn =
            connect(sourceModel, &QAbstractItemModel::modelAboutToBeReset, this, [this]() { beginResetModel(); });
        mModelResetConn = connect(sourceModel, &QAbstractItemModel::modelReset, this, [this]() { endResetModel(); });

        mLayoutAboutToBeChangedConn = connect(
            sourceModel, &QAbstractItemModel::layoutAboutToBeChanged, this,
            [this](
                const QList<QPersistentModelIndex> &parents, QAbstractItemModel::LayoutChangeHint hint
            ) { emit layoutAboutToBeChanged(parents, hint); }
        );
        mLayoutChangedConn = connect(
            sourceModel, &QAbstractItemModel::layoutChanged, this,
            [this](
                const QList<QPersistentModelIndex> &parents, QAbstractItemModel::LayoutChangeHint hint
            ) { emit layoutChanged(parents, hint); }
        );
    }

    endResetModel();
}

QModelIndex RowOrderProxyModel::index(int row, int column, const QModelIndex &parent) const
{
    if (parent.isValid())
    {
        return QModelIndex();
    }
    return createIndex(row, column);
}

QModelIndex RowOrderProxyModel::parent(const QModelIndex & /*child*/) const
{
    return QModelIndex();
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
        return QModelIndex();
    }
    if (sourceIndex.parent().isValid())
    {
        return QModelIndex();
    }
    if (!mReversed)
    {
        return createIndex(sourceIndex.row(), sourceIndex.column());
    }
    const int rows = sourceModel()->rowCount(QModelIndex());
    if (rows == 0)
    {
        return QModelIndex();
    }
    return createIndex(rows - 1 - sourceIndex.row(), sourceIndex.column());
}

QModelIndex RowOrderProxyModel::mapToSource(const QModelIndex &proxyIndex) const
{
    if (!proxyIndex.isValid() || sourceModel() == nullptr)
    {
        return QModelIndex();
    }
    if (!mReversed)
    {
        return sourceModel()->index(proxyIndex.row(), proxyIndex.column(), QModelIndex());
    }
    const int rows = sourceModel()->rowCount(QModelIndex());
    if (rows == 0)
    {
        return QModelIndex();
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
    // In reversed mode the source rectangle [tlRow..brRow] x
    // [tlCol..brCol] maps to proxy rectangle
    // [rows-1-brRow..rows-1-tlRow] x [tlCol..brCol]. Build the proxy
    // top-left and bottom-right manually so the row range is
    // re-anchored on the smaller-index corner along both axes.
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
    // mapping flip. The empty-source case is a degenerate flip;
    // emit the layout-change pair anyway so observers re-bind, then
    // skip the remap (no rows means no persistent indices to move).
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
