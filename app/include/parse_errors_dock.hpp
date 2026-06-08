#pragma once

#include <QDockWidget>
#include <QString>

#include <string>
#include <vector>

class QCloseEvent;
class QListWidget;
class QPushButton;
class QLabel;

/// Dockable panel that collects parse / open errors.
///
/// Replaces the modal `QMessageBox::warning` that used to surface
/// errors from `MainWindow::ShowParseErrors`; a persistent panel
/// fits streaming sessions where errors arrive continuously.
///
/// Position persists via `QMainWindow::saveState()` / `restoreState()`.
/// Entries are session-scoped: every destructive open path in
/// `MainWindow` calls `ResetSessionState()`. The live store is
/// capped at `MAX_DISPLAYED_ERRORS`; older entries are evicted and
/// a sticky overflow footer reports the dropped count.
class ParseErrorsDock : public QDockWidget
{
    Q_OBJECT

public:
    /// Hard cap on stored entries. Older ones are evicted and the
    /// count is surfaced in the summary.
    static constexpr int MAX_DISPLAYED_ERRORS = 1000;

    explicit ParseErrorsDock(QWidget *parent = nullptr);

    /// Append one batch of errors under @p title. No-op for empty
    /// @p errors. Fires `firstBatchArrived` once per session
    /// (cleared only by `ResetSessionState`) so a user who already
    /// dismissed the dock isn't yanked back to it mid-session.
    void AppendErrors(const QString &title, const std::vector<std::string> &errors);

    /// Drop every displayed entry. Does NOT re-arm
    /// `firstBatchArrived`; use `ResetSessionState` for that.
    void ClearErrors();

    /// Drop every entry AND re-arm `firstBatchArrived`. Called
    /// from every destructive session boundary in `MainWindow`.
    void ResetSessionState();

    /// Total entries currently displayed (excludes evicted). O(1).
    [[nodiscard]] int Count() const noexcept
    {
        return mErrorCount;
    }

    /// Entries evicted by `MAX_DISPLAYED_ERRORS` since the last
    /// `ClearErrors()`. Reflected in the summary header.
    [[nodiscard]] int DroppedCount() const noexcept
    {
        return mDroppedCount;
    }

signals:
    /// Emitted on every count change. The status-bar indicator listens
    /// here to hide itself when empty and to update its tooltip.
    void countChanged(int count, int droppedCount);

    /// First batch after construction or `ResetSessionState`.
    /// `MainWindow` decides whether to raise the dock (it shouldn't
    /// interrupt e.g. an in-progress find).
    void firstBatchArrived();

    /// Emitted on genuine user dismissal. See `FindDock::closed`
    /// for the rationale.
    void closed();

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    /// Refresh the header summary and emit `countChanged`.
    void RefreshSummary();

    /// Copy selected error rows (with synthesised headers and
    /// overflow footer) to the clipboard.
    void CopySelection() const;

    /// Evict oldest entries until the count is back under the cap.
    /// Re-mints the most recently evicted group header when its
    /// surviving rows would otherwise be stranded.
    void TrimToCap();

    /// Append the "...N more dropped" footer iff `mDroppedCount > 0`.
    /// Caller is responsible for stripping any prior footer first.
    void RebuildOverflowFooter();

    QListWidget *mList = nullptr;
    QLabel *mSummary = nullptr;
    QPushButton *mClearButton = nullptr;

    /// Running tally of error rows in `mList` so `Count()` stays O(1).
    int mErrorCount = 0;
    /// Cumulative evictions since the last `ClearErrors()`.
    int mDroppedCount = 0;
    /// First-batch latch; cleared only by `ResetSessionState`. Decoupled
    /// from the counts so the in-dock Clear button doesn't re-arm the
    /// auto-raise -- the user already signalled they're not interested.
    bool mHasSeenFirstBatch = false;
};
