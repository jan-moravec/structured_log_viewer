#pragma once

#include <QAbstractProxyModel>
#include <QList>
#include <QMetaObject>

/// Optional newest-first row-reversal layer between `LogModel` and
/// `LogFilterModel`:
///
/// ```
/// LogModel → RowOrderProxyModel → LogFilterModel → LogTableView
/// ```
///
/// Identity mapping when `mReversed` is false; mirrors source rows
/// around the row-count midpoint when true, so the newest source row
/// lands at proxy row 0.
///
/// **Why a custom proxy, not `QSortFilterProxyModel`**: the prior
/// implementation used `QSortFilterProxyModel` with
/// `setDynamicSortFilter(true)` plus `invalidate()` on every
/// `rowsInserted`. Each invalidation re-sorted the *entire* row list
/// (O(N log N) per batch), so streaming a 1 GB file scaled as
/// O(N² log N) and locked the GUI for tens of seconds. This proxy
/// does O(1)-per-row reverse mapping (no sort, no mapping table) and
/// forwards source signals with translated coordinates, so structural
/// changes scale linearly.
///
/// The downstream `LogFilterModel` filters and applies user-clicked
/// column sorts on the (already reversed) rows produced here, so the
/// two layers' sort states never overlap.
class RowOrderProxyModel : public QAbstractProxyModel
{
    Q_OBJECT

public:
    explicit RowOrderProxyModel(QObject *parent = nullptr);

    void setSourceModel(QAbstractItemModel *sourceModel) override;

    [[nodiscard]] QModelIndex index(int row, int column, const QModelIndex &parent = QModelIndex()) const override;
    [[nodiscard]] QModelIndex parent(const QModelIndex &child) const override;
    [[nodiscard]] int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    [[nodiscard]] int columnCount(const QModelIndex &parent = QModelIndex()) const override;

    [[nodiscard]] QModelIndex mapFromSource(const QModelIndex &sourceIndex) const override;
    [[nodiscard]] QModelIndex mapToSource(const QModelIndex &proxyIndex) const override;

    /// Toggle the reversed view. O(persistent indices), not O(rows):
    /// emits `layoutAboutToBeChanged` / `layoutChanged` and updates
    /// stored persistent indices via `changePersistentIndexList`.
    /// Idempotent.
    ///
    /// Production code goes through `MainWindow::ApplyDisplayOrder`
    /// instead, so proxy reversal, the `LogTableView` tail edge, and
    /// alternating-row colours all move together.
    void SetReversed(bool reversed);

    /// True if the proxy is currently reversed (newest-first).
    [[nodiscard]] bool IsReversed() const noexcept;

private:
    /// Forward `dataChanged(topLeft, bottomRight, roles)` from the
    /// source. Passthrough in identity mode; in reversed mode the row
    /// range is mirrored (columns untouched) so the proxy's
    /// `topLeft <= bottomRight` invariant holds along both axes.
    void HandleSourceDataChanged(const QModelIndex &topLeft, const QModelIndex &bottomRight, const QList<int> &roles);

    /// Drop only the source-model connections we own; the base class
    /// owns its own.
    void DisconnectSourceConnections();

    bool mReversed = false;

    /// Tracks whether the source's `columnsAboutToBeMoved` opened a
    /// `beginMoveColumns` pair, so the matching `columnsMoved` slot
    /// only calls `endMoveColumns` when there's something to close.
    /// Reset on every pair and on `setSourceModel`.
    bool mInSourceColumnMove = false;

    /// Connections to the source. Tracked so `setSourceModel` can
    /// disconnect them without touching the base class' wires.
    QMetaObject::Connection mRowsAboutToBeInsertedConn;
    QMetaObject::Connection mRowsInsertedConn;
    QMetaObject::Connection mRowsAboutToBeRemovedConn;
    QMetaObject::Connection mRowsRemovedConn;
    QMetaObject::Connection mColumnsAboutToBeInsertedConn;
    QMetaObject::Connection mColumnsInsertedConn;
    QMetaObject::Connection mColumnsAboutToBeRemovedConn;
    QMetaObject::Connection mColumnsRemovedConn;
    QMetaObject::Connection mColumnsAboutToBeMovedConn;
    QMetaObject::Connection mColumnsMovedConn;
    QMetaObject::Connection mDataChangedConn;
    QMetaObject::Connection mHeaderDataChangedConn;
    QMetaObject::Connection mModelAboutToBeResetConn;
    QMetaObject::Connection mModelResetConn;
    QMetaObject::Connection mLayoutAboutToBeChangedConn;
    QMetaObject::Connection mLayoutChangedConn;
};
