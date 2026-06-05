#pragma once

#include <QDockWidget>
#include <QString>

#include <string>
#include <vector>

class QCloseEvent;
class QListWidget;
class QPushButton;
class QLabel;

/// Dockable panel that shows accumulated parse / open errors.
///
/// Replaces the modal `QMessageBox::warning` that used to surface
/// errors from `MainWindow::ShowParseErrors`. A persistent panel
/// is better suited to streaming sessions where new errors can
/// arrive at any moment without the user wanting to dismiss a
/// dialog each time.
///
/// Position persists across runs via `QMainWindow::saveState()`
/// / `restoreState()` keyed on the dock's `objectName`. Entries
/// are session-scoped — every destructive open path in
/// `MainWindow` calls `ClearErrors()`.
///
/// To keep pathological streams (millions of malformed lines)
/// from OOM-ing the GUI, the live store is capped at
/// `MAX_DISPLAYED_ERRORS`. Past the cap the dock evicts oldest
/// entries and shows a sticky "and N more dropped" footer so the
/// user knows the count is honest.
class ParseErrorsDock : public QDockWidget
{
    Q_OBJECT

public:
    /// Hard cap on stored entries. Beyond this the oldest are
    /// evicted; the eviction count surfaces in the summary.
    static constexpr int MAX_DISPLAYED_ERRORS = 1000;

    explicit ParseErrorsDock(QWidget *parent = nullptr);

    /// Append one batch of errors under @p title. No-op for an
    /// empty @p errors. The dock auto-raises only on the first
    /// batch of a session (i.e. when the panel was previously
    /// empty); subsequent batches update silently and rely on the
    /// status-bar indicator to surface the new count. This avoids
    /// stealing focus from a streaming-session user who has
    /// already deliberately closed the dock.
    void AppendErrors(const QString &title, const std::vector<std::string> &errors);

    /// Drop every entry. Used when the user clicks the Clear
    /// button and from `MainWindow` on session discards.
    void ClearErrors();

    /// Total number of error entries currently displayed (does
    /// not include evicted entries; see @p DroppedCount). O(1).
    [[nodiscard]] int Count() const noexcept
    {
        return mErrorCount;
    }

    /// Number of entries evicted by the `MAX_DISPLAYED_ERRORS`
    /// cap since the last `ClearErrors()`. Reflected in the
    /// summary header.
    [[nodiscard]] int DroppedCount() const noexcept
    {
        return mDroppedCount;
    }

signals:
    /// Emitted whenever the displayed-error count or the dropped
    /// count changes. The status-bar indicator listens here so it
    /// can hide itself when the dock empties and surface both
    /// figures in its tooltip.
    void countChanged(int count, int droppedCount);

    /// Emitted on the first batch appended after a `ClearErrors()`
    /// (or since construction). `MainWindow` listens so it can
    /// decide whether to raise / show the dock, factoring in
    /// whether the user is currently doing something the auto-raise
    /// would interrupt (e.g. typing in the find bar). Moving the
    /// raise decision out of the dock keeps the dock unaware of
    /// the rest of the chrome.
    void firstBatchArrived();

    /// Emitted when the user actually dismisses the dock. Not
    /// emitted on tab inactivation -- see `FindDock::closed` for
    /// the same idiom and rationale.
    void closed();

protected:
    /// Emit `closed` once the base class has accepted the close so
    /// the View-menu checkmark can drop without false-firing on
    /// tab switches.
    void closeEvent(QCloseEvent *event) override;

private:
    /// Refresh the header summary (e.g. "12 entries") and emit
    /// `countChanged`.
    void RefreshSummary();

    /// Copy the selected error rows to the clipboard, one entry
    /// per line. Group headers and the overflow footer are
    /// included verbatim so the pasted block reads coherently.
    void CopySelection() const;

    /// Keep `mList` under the cap by evicting oldest entries
    /// (and any group header that becomes orphaned). Increments
    /// `mDroppedCount` per evicted error row.
    ///
    /// Does *not* manage the overflow footer -- that's
    /// `RebuildOverflowFooter`'s job. Splitting them matters
    /// because a single batch larger than the cap reaches
    /// `mErrorCount == MAX_DISPLAYED_ERRORS` exactly (the
    /// pre-trim in `AppendErrors` lops the surplus off the
    /// *input* before items are minted), so this method has no
    /// eviction work to do but the dock still owes the user a
    /// footer reflecting the pre-trimmed drops.
    void TrimToCap();

    /// Ensure the trailing overflow footer is present iff
    /// `mDroppedCount > 0`, with text reflecting the current
    /// count. Idempotent. The caller is responsible for having
    /// removed any prior footer (`AppendErrors` does this at the
    /// top of the function) so this method only ever *adds* a
    /// footer; it never has to compare-and-replace.
    void RebuildOverflowFooter();

    QListWidget *mList = nullptr;
    QLabel *mSummary = nullptr;
    QPushButton *mClearButton = nullptr;

    /// Running tally of error rows in `mList`. Maintained on
    /// every append / evict / clear so `Count()` stays O(1) and
    /// `RefreshSummary` doesn't walk the list.
    int mErrorCount = 0;
    /// Cumulative evictions since the last `ClearErrors()`.
    int mDroppedCount = 0;
};
