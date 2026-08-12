#pragma once

#include "scoped_connections.hpp"

#include <QDockWidget>
#include <QPersistentModelIndex>
#include <QPointer>

class AnchorManager;
class LogModel;
class LogSession;
class RecordDetailWidget;
class QCloseEvent;
struct SessionBindContext;

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
    /// `model` is borrowed and must outlive the dock. `anchors` is
    /// optional -- when supplied, the dock rebuilds its snapshot on
    /// anchor mutations so the anchor-note subline stays live.
    /// nullptr keeps the dock functional without the subline.
    RecordDetailDock(LogModel *model, AnchorManager *anchors = nullptr, QWidget *parent = nullptr);

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

    /// Task 5.7: swap the dock's active session context.
    ///
    /// Sequence (must stay this order):
    ///   1. Save the currently-pinned source row into the
    ///      outgoing session's `SessionRecordDetailPin`.
    ///   2. Reset the `QPersistentModelIndex` *before* swapping
    ///      the model pointer. Qt's persistent indexes are keyed
    ///      by their `QAbstractItemModel *`; letting one survive
    ///      into a new model risks a dangling reference the next
    ///      time the outgoing model emits `layoutChanged`.
    ///   3. Drop the scoped-connections bag so no in-flight slot
    ///      dereferences a stale `mModel` / `mAnchors`.
    ///   4. Swap the guarded aliases (`mModel`, `mAnchors`) from
    ///      the incoming context.
    ///   5. Reinstall the same subscriptions against the new
    ///      pointers via `InstallSourceSubscriptions()`.
    ///   6. Restore the incoming session's pin state (or show the
    ///      default "select a row" placeholder if there is nothing
    ///      to restore).
    ///
    /// A `SessionBindContext::MakeUnbound()` context detaches from
    /// every session-owned source and shows the default
    /// placeholder.
    void Bind(const SessionBindContext &context);

    /// Explicit unbind. Equivalent to `Bind(SessionBindContext::
    /// MakeUnbound())` but reads clearer at teardown call sites.
    void Unbind();

    /// Test seam: the session this dock currently mirrors state
    /// into on Bind / Unbind. Null before the first Bind or after
    /// an Unbind.
    [[nodiscard]] LogSession *boundSessionForTest() const noexcept
    {
        return mBoundSession.data();
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

    /// Install the source subscriptions (modelReset / rowsRemoved /
    /// dataChanged / columnsMoved / columnsInserted on the model,
    /// anchorChanged / anchorNoteChanged / anchorsReset on the
    /// anchor manager). Extracted so the ctor and `Bind` land the
    /// same wiring against a possibly-different source pair.
    void InstallSourceSubscriptions();

    /// Save the currently-pinned source row into the currently-
    /// bound session's `SessionRecordDetailPin`. No-op when no
    /// session is bound.
    void SaveStateIntoBoundSession();

    /// Reapply @p session's `SessionRecordDetailPin` -- pins the
    /// stored row (via `ShowSourceRow`) or shows the appropriate
    /// placeholder when nothing to restore.
    void RestoreStateFromSession(LogSession *session);

    QPointer<LogModel> mModel;
    QPointer<AnchorManager> mAnchors;
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

    /// Session-owned source subscriptions installed by
    /// `InstallSourceSubscriptions` and reaped on the next
    /// `Bind` (or on this dock's destruction). Explicitly
    /// excludes the widget subscriptions (openInNewWindow,
    /// dockLocationChanged, visibilityChanged) which live with
    /// the dock and must survive every rebind.
    ScopedConnections mSourceConnections;

    /// Session this dock currently mirrors state into. Null before
    /// the first `Bind` or after an explicit `Unbind`.
    QPointer<LogSession> mBoundSession;
#ifdef LOGAPP_BUILD_TESTING
    int mRefreshCount = 0;
#endif
};
