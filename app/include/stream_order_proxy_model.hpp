#pragma once

#include <QAbstractItemModel>
#include <QMetaObject>
#include <QSortFilterProxyModel>

/// Optional newest-first reversal layer between `LogModel` and
/// `LogFilterModel`:
///
/// ```
/// LogModel → StreamOrderProxyModel → LogFilterModel → LogTableView
/// ```
///
/// When `mReversed` is false the proxy is the identity. When true it
/// sorts by `InsertionOrderRole` descending so the most-recently-
/// appended source row lands at proxy row 0. The downstream
/// `LogFilterModel` filters and applies user-clicked column sorts on
/// the (already reversed) rows produced here, so the two layers'
/// sort states never overlap.
class StreamOrderProxyModel : public QSortFilterProxyModel
{
    Q_OBJECT

public:
    explicit StreamOrderProxyModel(QObject *parent = nullptr);

    void setSourceModel(QAbstractItemModel *sourceModel) override;

    /// Toggle the reversed view. Idempotent.
    ///
    /// Production code goes through `MainWindow::ApplyStreamingDisplayOrder`
    /// instead -- proxy reversal, `LogTableView` tail edge, and
    /// alternating-row colours must all move together.
    void SetReversed(bool reversed);

    /// Whether the proxy is currently reversed (newest-first).
    [[nodiscard]] bool IsReversed() const noexcept;

private:
    bool mReversed = false;

    /// Our own source-model connections, tracked so `setSourceModel`
    /// can disconnect them without touching the base class' wires.
    QMetaObject::Connection mRowsInsertedConn;
    QMetaObject::Connection mRowsRemovedConn;
};
