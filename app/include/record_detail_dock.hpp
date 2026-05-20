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
///     bails instead of snapshotting a different record. The dock
///     also swaps to `EvictedRecordPlaceholder()` so the user can
///     tell "I never picked a record" apart from "what I picked is
///     gone";
///   - if rows are evicted from the front while the pinned row
///     survives, the persistent index follows the shift, so the
///     content stays bound to the same logical record.
/// The dock also listens to `rowsRemoved` to refresh its display
/// when the pinned row is still alive but its position changed (the
/// summary label uses the row number). Refresh work is gated on
/// `isHidden()`: a closed dock skips the rebuild, and
/// `MainWindow::UpdateRecordDetailsFromSelection` re-pins us from
/// the table's current selection the next time the dock is shown.
/// The eviction placeholder swap stays unconditional so the next
/// show surfaces the correct state without further plumbing.
///
/// Pinning column 0 in the persistent index assumes `LogModel`
/// never removes columns. Today only `beginInsertColumns` is
/// emitted (see `app/src/log_model.cpp`); a future column-removal
/// path would need to rework the column part of the pin.
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

    /// Reset to the "no row" placeholder and forget any prior pin.
    /// Called from `LogModel::modelReset`; also from `ShowSourceRow`
    /// when the requested row is out of range.
    void Clear();

    /// Current source row, or -1 if the dock holds a placeholder
    /// (or the persistent index went invalid because the pinned row
    /// was evicted).
    [[nodiscard]] int CurrentSourceRow() const noexcept;

    [[nodiscard]] RecordDetailWidget *Widget() const noexcept
    {
        return mWidget;
    }

    /// Combined "should we pay for a content refresh right now"
    /// gate. `isHidden()` alone is insufficient: in a tabified
    /// dock area the dock can be `!isHidden()` while its tab is
    /// buried behind another dock's tab, in which case
    /// `visibilityChanged(false)` has fired but the explicit-hide
    /// flag is still false. We track that secondary state via
    /// `visibilityChanged` so the gate skips work the user can't
    /// see. The flag defaults to `true` so tests that never
    /// realise the parent (offscreen QPA, no `MainWindow::show()`)
    /// keep the in-process flow working off the cheaper
    /// `isHidden()` check alone.
    ///
    /// Exposed publicly so the host (`MainWindow`) can short-circuit
    /// selection-driven refreshes too; the in-dock listeners use
    /// the same gate internally.
    [[nodiscard]] bool IsVisibleForRefresh() const noexcept;

signals:
    /// User clicked the widget's "Open in new window" button. The
    /// argument is the currently-shown source row, or -1 when the
    /// dock holds a placeholder (in which case `MainWindow` should
    /// ignore the request).
    void openInNewWindowRequested(int sourceRow);

#ifdef LOGAPP_BUILD_TESTING
public:
    /// Test-only counter incremented on every successful
    /// `RefreshFromModel`. Lets gating regression tests
    /// (e.g. "this `dataChanged` outside the pinned row must NOT
    /// rebuild") observe the cheap-skip path directly instead of
    /// inferring it from structural-equality asserts on the
    /// resulting `RecordDetailContent`.
    [[nodiscard]] int RefreshCountForTest() const noexcept
    {
        return mRefreshCount;
    }
#endif

private:
    void RefreshFromModel();
    void OnOpenInNewWindowRequested();

    /// Swap to `EvictedRecordPlaceholder()` without touching
    /// `mEverPinned`. We stay in the "was pinned, then evicted"
    /// state until something else (`Clear`, a fresh `ShowSourceRow`)
    /// transitions us out, so consecutive `rowsRemoved` events
    /// don't ping-pong placeholder text.
    void ShowEvictedPlaceholder();

    QPointer<LogModel> mModel;
    RecordDetailWidget *mWidget = nullptr;
    /// Pinned row as a persistent index against the source model.
    /// Invalid means "no row pinned" or "the pinned row was
    /// evicted". `Qt::DisplayRole` etc. are read through
    /// `BuildRecordDetailContent` so we always reflect the latest
    /// table state when refreshing.
    QPersistentModelIndex mCurrentSourceIndex;
    /// True from the first successful `ShowSourceRow` until the
    /// next `Clear`. Lets the `rowsRemoved` handler distinguish
    /// "no pin to begin with" (no-op) from "the pinned record was
    /// just evicted" (swap to `EvictedRecordPlaceholder`). Without
    /// this flag both states are observationally identical because
    /// `mCurrentSourceIndex.isValid()` is false in both.
    bool mEverPinned = false;
    /// Tracks `QDockWidget::visibilityChanged`. Stays true under
    /// offscreen QPA where the signal never fires (no realised
    /// parent), which keeps tests using `RecordDetailDock dock(&model);
    /// dock.show();` working off the `isHidden()` gate. In a real
    /// session this flips to false when the dock's tab is buried.
    bool mPerceivedVisible = true;
#ifdef LOGAPP_BUILD_TESTING
    int mRefreshCount = 0;
#endif
};
