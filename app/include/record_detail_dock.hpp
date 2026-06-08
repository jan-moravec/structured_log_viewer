#pragma once

#include <QDockWidget>
#include <QPersistentModelIndex>
#include <QPointer>

class LogModel;
class RecordDetailWidget;
class QCloseEvent;

/// Dockable host for a single `RecordDetailWidget`. Owned by
/// `MainWindow`; lives next to the central table view.
///
/// The pinned row is tracked as a `QPersistentModelIndex` against
/// the source model. Qt keeps it in lockstep with row insertions and
/// removals (FIFO eviction in streaming mode):
///   - if the pinned row survives an eviction, the index shifts with
///     it and the content stays bound to the same record;
///   - if it is evicted, the index goes invalid, `CurrentSourceRow()`
///     returns -1, and the dock swaps to `EvictedRecordPlaceholder()`
///     so the user can tell "never picked anything" from "the record
///     I picked is gone".
///
/// Refresh work is gated on `IsVisibleForRefresh()` so a hidden /
/// buried-tab dock skips rebuilds; the visibility hook refreshes once
/// on re-surface, and `MainWindow::UpdateRecordDetailsFromSelection`
/// re-pins from the selection.
///
/// Pinning column 0 assumes `LogModel` never removes columns (today
/// only `beginInsertColumns` is emitted).
class RecordDetailDock : public QDockWidget
{
    Q_OBJECT

public:
    /// `model` is borrowed and must outlive the dock.
    RecordDetailDock(LogModel *model, QWidget *parent = nullptr);

    /// Pin to @p sourceRow and refresh. Out-of-range rows clear the
    /// view.
    void ShowSourceRow(int sourceRow);

    /// Reset to the default placeholder and forget any prior pin.
    void Clear();

    /// Current source row, or -1 if no row is pinned (e.g. the
    /// pinned row was evicted).
    [[nodiscard]] int CurrentSourceRow() const noexcept;

    [[nodiscard]] RecordDetailWidget *Widget() const noexcept
    {
        return mWidget;
    }

    /// Should we pay for a refresh right now? Combines `isHidden()`
    /// with the tracked `visibilityChanged` state so a tabified dock
    /// whose tab is buried also skips work. Defaults to true so tests
    /// that never realise the parent (offscreen QPA) keep working off
    /// the `isHidden()` check alone. Public so `MainWindow` can apply
    /// the same gate to selection-driven refreshes.
    [[nodiscard]] bool IsVisibleForRefresh() const noexcept;

signals:
    /// User clicked "Open in new window". Argument is the current
    /// source row, or -1 when no row is pinned.
    void openInNewWindowRequested(int sourceRow);

    /// Emitted on genuine user dismissal (X button, system close).
    /// Distinct from `visibilityChanged(false)`, which also fires on
    /// tab inactivation in a tabified group.
    void closed();

protected:
    void closeEvent(QCloseEvent *event) override;

#ifdef LOGAPP_BUILD_TESTING
public:
    /// Counter for `RefreshFromModel`. Lets gating tests observe the
    /// cheap-skip path directly rather than inferring it from content
    /// equality (`SetContent` is idempotent, so equal payloads can't
    /// distinguish "skipped" from "rebuilt to the same value").
    [[nodiscard]] int RefreshCountForTest() const noexcept
    {
        return mRefreshCount;
    }
#endif

private:
    void RefreshFromModel();
    void OnOpenInNewWindowRequested();

    /// Swap to `EvictedRecordPlaceholder()` without clearing
    /// `mEverPinned`, so consecutive `rowsRemoved` events don't
    /// ping-pong the placeholder text.
    void ShowEvictedPlaceholder();

    QPointer<LogModel> mModel;
    RecordDetailWidget *mWidget = nullptr;
    /// Persistent pin against the source model. Invalid means no
    /// pin, or the pinned row was evicted.
    QPersistentModelIndex mCurrentSourceIndex;
    /// True from the first successful `ShowSourceRow` until the next
    /// `Clear`. Distinguishes "never pinned" from "was pinned, then
    /// evicted" in the `rowsRemoved` handler -- both leave
    /// `mCurrentSourceIndex` invalid.
    bool mEverPinned = false;
    /// Tracks `visibilityChanged`. Stays true under offscreen QPA
    /// (signal never fires there); flips false in a real session
    /// when the dock's tab is buried.
    bool mPerceivedVisible = true;
#ifdef LOGAPP_BUILD_TESTING
    int mRefreshCount = 0;
#endif
};
