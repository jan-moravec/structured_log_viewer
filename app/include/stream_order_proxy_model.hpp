#pragma once

#include <QAbstractItemModel>
#include <QMetaObject>
#include <QSortFilterProxyModel>

/// Thin proxy layer that lets the user view a stream **newest-first**
/// (reversed) without disturbing the outer `LogFilterModel`'s
/// user-clicked column sort. Sits between `LogModel` and
/// `LogFilterModel` in the model chain:
///
/// ```
/// LogModel → StreamOrderProxyModel → LogFilterModel → LogTableView
/// ```
///
/// In **identity mode** (default; `mReversed == false`) the proxy
/// passes the source order through verbatim — row N of the proxy maps
/// to row N of the source.
///
/// In **reversed mode** (`mReversed == true`) the proxy sorts by
/// `LogModelItemDataRole::InsertionOrderRole` with
/// `Qt::DescendingOrder`, so the highest source row index (the most-
/// recently-appended streamed line) lands at proxy row 0 and the
/// oldest row at the bottom of the view. Filtering and per-column
/// user sorts on the downstream `LogFilterModel` continue to work
/// unchanged because their proxy operates on the (already-reversed)
/// rows produced by this layer.
///
/// The reversed mode is persisted via `StreamingControl::IsNewestFirst()`
/// and toggled at runtime from `PreferencesEditor`. `MainWindow` is
/// responsible for keeping the `LogTableView` "tail edge" (top vs.
/// bottom) and the Follow newest scroll direction in sync with the
/// proxy's reversed flag.
class StreamOrderProxyModel : public QSortFilterProxyModel
{
    Q_OBJECT

public:
    explicit StreamOrderProxyModel(QObject *parent = nullptr);

    void setSourceModel(QAbstractItemModel *sourceModel) override;

    /// Toggles the reversed view. No-op when @p reversed already matches
    /// the current state. Triggers Qt's standard sort/refresh signals so
    /// any attached view updates immediately.
    ///
    /// **Do not call this directly from production code** — go through
    /// `MainWindow::ApplyStreamingDisplayOrder`, which is the single
    /// sync point for the newest-first orientation (proxy reversal,
    /// `LogTableView` tail edge, and alternating-row-colours toggle
    /// all need to move together; calling `SetReversed` in isolation
    /// leaves the table view's Follow-tail anchor pointing at the
    /// wrong edge).
    void SetReversed(bool reversed);

    /// Whether the proxy is currently in the reversed (newest-first)
    /// view. GUI-thread only.
    [[nodiscard]] bool IsReversed() const noexcept;

private:
    bool mReversed = false;

    /// Source-model `rowsInserted` / `rowsRemoved` connections we add
    /// on top of `QSortFilterProxyModel`'s own internal plumbing.
    /// Tracked as handles so `setSourceModel` can disconnect just our
    /// wires without disturbing the base class' signal hookups.
    QMetaObject::Connection mRowsInsertedConn;
    QMetaObject::Connection mRowsRemovedConn;
};
