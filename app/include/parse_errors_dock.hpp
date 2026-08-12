#pragma once

#include "log_session_presentation.hpp"

#include <QDockWidget>
#include <QPointer>
#include <QString>

#include <string>
#include <vector>

class QCloseEvent;
class QListWidget;
class QPushButton;
class QLabel;
class LogSession;
struct SessionBindContext;

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
    ///
    /// Equivalent to `AppendErrorsForSession(nullptr, title,
    /// errors)`: writes to whichever session is currently bound
    /// (matches the phase-5 single-session shell). Phase 6 tab-
    /// switch callers that know the ORIGINATING session (e.g. an
    /// asynchronous parse completion on a background tab) MUST
    /// use the four-arg overload so the batch lands in the
    /// originating session's log, not the currently-active one.
    void AppendErrors(const QString &title, const std::vector<std::string> &errors);

    /// Append one batch of errors under @p title, attributing the
    /// batch to @p originating.
    ///
    /// Behavior matrix (originating vs. currently-bound):
    /// * `originating == nullptr` OR `originating == boundSession`:
    ///   full path -- visible list + shadow + counters + session
    ///   log update. Fires `firstBatchArrived` on the first batch.
    /// * `originating != nullptr && originating != boundSession`:
    ///   log-only path -- mirror the batch (with the same
    ///   pre-trim + trim-to-cap semantics) into the originating
    ///   session's `SessionParseErrorLog` so a subsequent Bind
    ///   back to that session replays the batch into the visible
    ///   list. Visible list, shadow, counters, and
    ///   `firstBatchArrived` are UNTOUCHED so the current tab's
    ///   UX is not perturbed.
    ///
    /// Origin-review finding H3: without the originating-session
    /// routing, a background parse completion on session A while
    /// session B is active would silently append A's errors under
    /// B's list AND persist them into B's log on the next Bind.
    /// Phase 6 tab switch would then surface A's errors under B's
    /// tab.
    void AppendErrorsForSession(LogSession *originating, const QString &title, const std::vector<std::string> &errors);

    /// Drop every displayed entry. Does NOT re-arm
    /// `firstBatchArrived`; use `ResetSessionState` for that.
    void ClearErrors();

    /// Drop every entry AND re-arm `firstBatchArrived`. Called
    /// from every destructive session boundary in `MainWindow`.
    /// If the dock is currently `Bind()`-ed to a session, the
    /// session's `SessionParseErrorLog` is also reset (task 5.4).
    void ResetSessionState();

    /// Bind to the session in @p context (task 5.4).
    ///
    /// Snapshots the currently-bound session's visible state into
    /// its `SessionParseErrorLog` (via `LogSession::
    /// MutableParseErrorLog()`), clears the visible list, then
    /// replays the incoming session's log so a phase-6 tab switch
    /// back to a previously-bound session restores every entry,
    /// the running counts, and the auto-raise latch exactly.
    ///
    /// Safe to call with `context.IsBound() == false`: that path
    /// snapshots + clears without loading a new log, i.e.
    /// equivalent to `Unbind()`. Idempotent for the same session:
    /// a re-Bind of the currently-bound session round-trips the
    /// visible state through its log without change.
    ///
    /// The dock does not subscribe to any session-owned signals
    /// here; error batches arrive via `AppendErrors(...)` from
    /// the shell's parse / streaming completion paths, which
    /// still routes through the currently-active session in
    /// phase 5.
    void Bind(const SessionBindContext &context);

    /// Snapshot into the currently-bound session (if any) and
    /// clear the visible list. Equivalent to
    /// `Bind(SessionBindContext::MakeUnbound())` but reads
    /// clearer at teardown call sites.
    void Unbind();

    /// The `LogSession` currently bound (or null if unbound).
    /// Exposed for tests that pin the save-outgoing / restore-
    /// incoming pattern; production callers should not use this.
    [[nodiscard]] LogSession *boundSessionForTest() const noexcept
    {
        return mBoundSession.data();
    }

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

    /// Persist every appended batch alongside the visible list
    /// so `Bind` / `Unbind` can round-trip the state through the
    /// bound `LogSession`'s `SessionParseErrorLog`. The visible
    /// list already trims to `MAX_DISPLAYED_ERRORS`; the shadow
    /// tracks the same trimmed content -- entries evicted by
    /// `TrimToCap` are dropped from both here and `mList` in
    /// lockstep so a save-and-restore cycle reproduces exactly
    /// what the user last saw.
    void MirrorAppendIntoShadow(const QString &title, const std::vector<std::string> &errors, size_t errorsBegin);
    void TrimShadowToCap();
    void ReplayLogIntoVisibleList(const SessionParseErrorLog &log);

    /// Append @p errors under @p title into @p session's log
    /// (via `SessionParseErrorLog`) WITHOUT touching the dock's
    /// visible list, shadow, counters, or first-batch latch.
    /// Applies the same pre-trim (surplus errors that would
    /// overflow `MAX_DISPLAYED_ERRORS`) + trim-to-cap FIFO
    /// eviction the visible path uses, so a subsequent Bind
    /// back to @p session sees a log identical to what would
    /// have been visible if the batch had landed while bound.
    ///
    /// Origin-review finding H3 backing store: keeps the
    /// per-session log authoritative even for batches that
    /// arrive while the session is in the background.
    void AppendErrorsIntoSessionLog(LogSession *session, const QString &title, const std::vector<std::string> &errors);

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

    /// Currently-bound session (task 5.4). Null before the first
    /// `Bind()` and after `Unbind()`. Used to route the save-
    /// outgoing snapshot on a `Bind` / `Unbind` / destructive
    /// session-reset call.
    QPointer<LogSession> mBoundSession;

    /// Shadow store of every batch that arrived via
    /// `AppendErrors` since the last `ClearErrors()` /
    /// `ResetSessionState()`, in insertion order. Held on the
    /// dock so `Bind` / `Unbind` can move it into (and out of)
    /// the bound session's `SessionParseErrorLog` in one hop
    /// without walking `mList` and re-parsing group headers
    /// (which are indistinguishable from ordinary rows once
    /// the overflow footer and per-batch header logic have run).
    std::vector<SessionParseErrorBatch> mBatchShadow;
};
