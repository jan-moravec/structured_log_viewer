#pragma once

#include <QDockWidget>
#include <QPointer>

class FindRecordWidget;
class LogSession;
class QCloseEvent;
class QShowEvent;
class QWidget;
struct SessionBindContext;

/// Dockable host for `FindRecordWidget`. Position persists via
/// `QMainWindow::saveState()` / `restoreState()`.
///
/// Allowed areas are top + bottom only; a vertical side dock would
/// squeeze the search field into an unusable narrow column.
class FindDock : public QDockWidget
{
    Q_OBJECT

public:
    explicit FindDock(QWidget *parent = nullptr);

    /// The hosted find widget. Borrow only; the dock owns it.
    [[nodiscard]] FindRecordWidget *Widget() const noexcept
    {
        return mWidget;
    }

    /// Show + raise the dock and focus the search field. Idempotent.
    /// On every call, stashes the previously-focused widget (when
    /// outside our subtree) so dismissing the bar can restore it.
    void RevealAndFocus();

    /// Bind to the session in @p context (task 5.3).
    ///
    /// Snapshots the outgoing session's query state (query text +
    /// wildcards/regex toggles) into its
    /// `SessionFindQueryState`, restores the incoming session's
    /// state onto `FindRecordWidget`, and cancels any in-flight
    /// debounce timers so a `MatchCountRequested` cannot fire
    /// against a stale model between the state restore and the
    /// next debounce arm. Safe to call with
    /// `context.IsBound() == false`: that path snapshots + clears
    /// without loading a new query (equivalent to `Unbind()`).
    /// Idempotent for the same session -- a re-Bind of the
    /// currently-bound session round-trips the query state
    /// through its store without change.
    ///
    /// Match-count state is not persisted: the debounce arm
    /// on restore fires a `MatchCountRequested` with the
    /// restored query against the (already-active) model, so
    /// the "*i* of *N*" label refreshes on the next quiet
    /// window without persisting a stale count that would need
    /// invalidating on every proxy mutation.
    void Bind(const SessionBindContext &context);

    /// Snapshot into the currently-bound session (if any), cancel
    /// the debounce, and clear the visible query. Equivalent to
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

signals:
    /// Emitted on genuine user dismissal (X button, `close()` from
    /// `DismissBar`, system close). Distinct from
    /// `visibilityChanged(false)`, which also fires on tab inactivation.
    void closed();

    /// Emitted when the bar becomes visible (cold reveal or tab
    /// activation). Named alias for `visibilityChanged(true)` that
    /// keeps the wiring self-documenting.
    void revealed();

protected:
    /// Restore focus to the stashed widget, then emit `closed`.
    void closeEvent(QCloseEvent *event) override;

    /// Emit `revealed` so listeners can refresh state that went stale
    /// while the bar was hidden / tab-buried.
    void showEvent(QShowEvent *event) override;

private:
    FindRecordWidget *mWidget = nullptr;

    /// Widget that held focus before the last reveal. `QPointer`
    /// guards against the widget being destroyed while the bar is open.
    QPointer<QWidget> mFocusBeforeReveal;

    /// Currently-bound session (task 5.3). Null before the first
    /// `Bind()` and after `Unbind()`.
    QPointer<LogSession> mBoundSession;
};
