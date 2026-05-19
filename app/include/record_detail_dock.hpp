#pragma once

#include <QDockWidget>
#include <QPersistentModelIndex>
#include <QPointer>

class LogModel;
class RecordDetailWidget;

/// Dockable host for a single `RecordDetailWidget`. Owned by
/// `MainWindow`; lives next to the central table view.
///
/// The dock follows the table's currently-selected row when visible:
/// `MainWindow` calls `ShowSourceRow(int)` on selection changes and
/// `Clear()` on `modelReset`. The widget always reads through
/// `BuildRecordDetailContent` from the live `LogModel`, so streaming
/// FIFO eviction is reflected the next time the selection updates.
///
/// The pinned row is tracked as a `QPersistentModelIndex` against the
/// source model, not as a raw int. Qt keeps the index in lockstep
/// with `rowsInserted` / `rowsRemoved` (FIFO eviction in streaming
/// mode), so:
///   - if the pinned row is evicted, the index goes invalid and
///     `CurrentSourceRow()` returns -1 -- "Open in new window" then
///     bails instead of snapshotting a different record;
///   - if rows are evicted from the front while the pinned row
///     survives, the persistent index follows the shift, so the
///     content stays bound to the same logical record.
/// The dock also listens to `rowsRemoved` to refresh its display
/// when the pinned row is still alive but its position changed (the
/// summary label uses the row number).
class RecordDetailDock : public QDockWidget
{
    Q_OBJECT

public:
    /// `model` is borrowed; must outlive the dock. `parent` becomes
    /// the `QDockWidget`'s parent (typically `MainWindow`).
    RecordDetailDock(LogModel *model, QWidget *parent = nullptr);

    /// Pin the dock to @p sourceRow of the source model and refresh
    /// the content. Negative or out-of-range rows clear the view.
    void ShowSourceRow(int sourceRow);

    /// Reset to the "no row" placeholder. Called from
    /// `LogModel::modelReset`.
    void Clear();

    /// Current source row, or -1 if the dock holds a placeholder
    /// (or the persistent index went invalid because the pinned row
    /// was evicted).
    [[nodiscard]] int CurrentSourceRow() const noexcept;

    [[nodiscard]] RecordDetailWidget *Widget() const noexcept
    {
        return mWidget;
    }

signals:
    /// User clicked the widget's "Open in new window" button. The
    /// argument is the currently-shown source row, or -1 when the
    /// dock holds a placeholder (in which case `MainWindow` should
    /// ignore the request).
    void openInNewWindowRequested(int sourceRow);

private:
    void RefreshFromModel();
    void OnOpenInNewWindowRequested();

    QPointer<LogModel> mModel;
    RecordDetailWidget *mWidget = nullptr;
    /// Pinned row as a persistent index against the source model.
    /// Invalid means "no row pinned" or "the pinned row was
    /// evicted". `Qt::DisplayRole` etc. are read through
    /// `BuildRecordDetailContent` so we always reflect the latest
    /// table state when refreshing.
    QPersistentModelIndex mCurrentSourceIndex;
};
