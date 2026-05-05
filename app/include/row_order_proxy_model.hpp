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
/// When `mReversed` is false the proxy is the identity. When true the
/// proxy mirrors the source rows around the row-count midpoint so the
/// most-recently-appended source row lands at proxy row 0.
///
/// **Why a custom `QAbstractProxyModel` rather than
/// `QSortFilterProxyModel`**: the prior implementation used
/// `QSortFilterProxyModel` with `setDynamicSortFilter(true)` plus an
/// explicit `invalidate()` on every `rowsInserted`. Each `invalidate()`
/// re-sorted the *entire* row list — O(N log N) per batch — so streaming
/// a 1 GB file scaled as O(N² log N) and locked the GUI for tens of
/// seconds. This proxy does **O(1) per-row** reverse mapping (no sort,
/// no internal mapping table) and forwards source signals with translated
/// row coordinates so structural changes scale linearly.
///
/// The downstream `LogFilterModel` (a `QSortFilterProxyModel`) filters
/// and applies user-clicked column sorts on the (already reversed) rows
/// produced here, so the two layers' sort states never overlap.
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
    /// instead -- proxy reversal, `LogTableView` tail edge, and
    /// alternating-row colours must all move together.
    void SetReversed(bool reversed);

    /// Whether the proxy is currently reversed (newest-first).
    [[nodiscard]] bool IsReversed() const noexcept;

private:
    /// Forward `dataChanged(topLeft, bottomRight, roles)` from the
    /// source. In identity mode this is a straight passthrough; in
    /// reversed mode the row range is mirrored (column range stays
    /// untouched) so the proxy's `topLeft <= bottomRight` invariant
    /// holds along both axes.
    void HandleSourceDataChanged(const QModelIndex &topLeft, const QModelIndex &bottomRight, const QList<int> &roles);

    /// Drop only the source-model connections we own. The base class
    /// owns its own; clearing both keeps signal forwarding clean
    /// across `setSourceModel` calls.
    void DisconnectSourceConnections();

    bool mReversed = false;

    /// Captured by `rowsAboutToBeRemoved` so the matching
    /// `rowsRemoved` call has the pre-removal source row count needed
    /// to compute the proxy-side range we already reported in
    /// `beginRemoveRows`. The value is meaningless outside the
    /// remove-rows window.
    int mPreRemovalSourceRows = 0;

    /// Connections to the source model. Tracked so `setSourceModel`
    /// can disconnect them without touching the base class' wires.
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
