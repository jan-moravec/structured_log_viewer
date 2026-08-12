#pragma once

#include <QPointer>
#include <QString>
#include <QWidget>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class LogSession;
class LogTableView;
class OverviewRailModel;
class OverviewRailWidget;
class ThemeControl;
class QAbstractItemDelegate;
class QLabel;
class QProgressBar;
class QPushButton;
class QVBoxLayout;
class QWidget;

/// Per-tab visual workspace bound to exactly one `LogSession`.
///
/// Phase 3 (task 3.1) turns this from a compile-only skeleton into
/// the widget that owns the session's `LogTableView`,
/// `OverviewRailModel`, and `OverviewRailWidget`. The shell keeps
/// non-owning aliases to those instances via
/// `Session()->Model()` / `Session()->FilterProxy()` and the
/// `TableView()` / `OverviewRail()` / `OverviewRailModel()`
/// accessors exposed here.
///
/// Ownership contract (PRD §4.2 and task 3.1):
///
/// - Constructed with a live `LogSession` pointer and never
///   transplanted to another session for its lifetime.
/// - Must not be implemented as a nested `QMainWindow`; the tab
///   container hosts plain widgets so shell chrome (menus, docks,
///   toolbars, status bar) stays single-instance per window.
/// - Owns the per-session widgets (`LogTableView`,
///   `OverviewRailWidget`) via Qt parentage so their lifetime
///   ends with this widget's, not with the shell's.
class LogSessionView : public QWidget
{
    Q_OBJECT

public:
    /// Phase-3 constructor. The @p theme argument is optional; when
    /// null the overview rail falls back to `QPalette::Highlight`
    /// (same behaviour as the shell used to install directly).
    /// A null @p session is a programming error in production;
    /// tests catch it early via `Q_ASSERT_X`. Widgets constructed
    /// here are parented on this view so the whole subtree tears
    /// down with the tab.
    LogSessionView(LogSession *session, ThemeControl *theme, QWidget *parent = nullptr);

    /// Convenience overload that omits the theme (used by unit
    /// tests that do not exercise theme-dependent painting).
    explicit LogSessionView(LogSession *session, QWidget *parent = nullptr);
    ~LogSessionView() override;

    LogSessionView(const LogSessionView &) = delete;
    LogSessionView &operator=(const LogSessionView &) = delete;
    LogSessionView(LogSessionView &&) = delete;
    LogSessionView &operator=(LogSessionView &&) = delete;

    /// The session this view is bound to for its whole lifetime.
    /// May return null after the session has been destroyed under
    /// teardown; docks and dialogs check before dereferencing.
    [[nodiscard]] LogSession *Session() const noexcept
    {
        return mSession.data();
    }

    /// Owned table view (created in the ctor when a session was
    /// Owned log table view. Non-null after construction; both the
    /// themed and theme-less ctor overloads delegate to
    /// `Initialise`, which unconditionally allocates the table.
    /// Callers must not `setParent(nullptr)` this pointer -- the
    /// widget is a child of this view and reparenting it would
    /// leak the layout child slot.
    [[nodiscard]] LogTableView *TableView() const noexcept
    {
        return mTableView;
    }

    /// Owned overview rail widget. Hidden by default; the shell's
    /// toggle action drives its visibility. See
    /// `LogTableView::AttachOverviewRail` for the reserve-margin
    /// dance that the visibility toggle drives (attach reparents
    /// the rail into the table's reserved right viewport margin;
    /// detach + `setParent(mSessionView)` restores it as a bare
    /// child of the view).
    [[nodiscard]] OverviewRailWidget *OverviewRail() const noexcept
    {
        return mOverviewRailWidget;
    }

    /// The rail's underlying bucket model. Exposed so the shell
    /// can wire `bucketsChanged` / find-tick rebroadcast without
    /// reaching through the widget.
    [[nodiscard]] OverviewRailModel *OverviewRailModelPtr() const noexcept
    {
        return mOverviewRailModel;
    }

    // -----------------------------------------------------------------
    // Task 3.4 (partial): navigation entry points migrated from
    // `MainWindow`. Signal-emitting variants let the shell keep
    // status-bar messages and the `actionFollowTail` QAction on
    // its side (they belong to shell chrome, not the view).
    // -----------------------------------------------------------------

    /// Select @p sourceRow (a `LogModel` source-row index) in the
    /// table via the proxy chain and centre-scroll to it. Emits
    /// `rowNotVisible()` when the row is out of range or filtered
    /// out; the shell surfaces that via `statusBar()`. Migrated
    /// from `MainWindow::SelectSourceRow` -- shell forwarder kept
    /// so existing signal connections (anchors dock, histogram
    /// dock) continue to work.
    void SelectSourceRow(int sourceRow);

    /// Centre-scroll to a proxy-row index (rail navigation entry
    /// point). @p replaceSelection true means fresh click semantics
    /// (clear + select); false means drag-scrub semantics (scroll
    /// only). Emits `followTailDisengageRequested()` on entry so
    /// the shell can uncheck `actionFollowTail` before a live-tail
    /// batch yanks the viewport back. Migrated from
    /// `MainWindow::ScrollToProxyRow`.
    void ScrollToProxyRow(int proxyRow, bool replaceSelection);

    // -----------------------------------------------------------------
    // Task 3.5 (partial): header-visibility apply migrated from
    // `MainWindow`. Shell wrappers add the find-cache invalidation
    // and filter-indicator refresh that live on shell chrome.
    // -----------------------------------------------------------------

    /// Apply the session model's per-column `visible` flags to the
    /// table's horizontal header, hiding sections whose column
    /// entry is not visible. Idempotent; safe to call after
    /// `modelReset` / configuration-load / column-recovery paths.
    /// No-op when the header or model is not yet wired.
    void ApplyColumnVisibility();

    /// Install (or detach) @p delegate on the level column of the
    /// table. Delegate ownership stays with the shell (the delegate
    /// is a per-window resource; Qt keeps it alive via the shell's
    /// object tree). The view tracks the currently-installed
    /// column so a reload that moves the level column detaches the
    /// old column before reinstalling on the new one.
    ///
    /// - Pass a non-null @p delegate to install on the model's
    ///   first level column when icon mode is active; text mode
    ///   detaches so paints skip the proxy-chain walk.
    /// - Pass nullptr to force detach (used by the no-theme test
    ///   fixture that has no delegate to install).
    void ApplyLevelCellDelegate(QAbstractItemDelegate *delegate);

    // -----------------------------------------------------------------
    // Task 3.6: Go to Line / Go to Timestamp dialogs migrated from
    // `MainWindow`. The view owns the modal + sticky-input state;
    // status feedback flows through `statusMessageRequested()` so
    // the shell can decide where to render (currently
    // `statusBar()->showMessage`).
    // -----------------------------------------------------------------

    /// Result of `ParseGotoTimestampInput`. `micros` is epoch
    /// microseconds; `isNaive` is true when the winning format had
    /// no zone specifier and the caller must therefore shift the
    /// value through the display TZ before comparing against
    /// stored (UTC-normalised) timestamps. The relative-shortcut
    /// path always returns `isNaive == false` since it derives
    /// from `system_clock::now()` (already UTC).
    struct GotoTimestampParse
    {
        int64_t micros = 0;
        bool isNaive = false;
    };

    /// Pop the "Go to Line" modal (`Ctrl+G`). One-based row number
    /// where 1 is always the earliest retained row. Rejects and
    /// status-hints on any error; a valid row hands off to
    /// `SelectSourceRow`. Post-dialog work lives in
    /// `ExecuteGotoLine` so tests can drive it without a modal.
    void PromptGotoLine();

    /// Pop the "Go to Timestamp..." modal (`Ctrl+Shift+G`). Accepts
    /// the current time column's `parseFormats`, two ISO
    /// fallbacks, and the relative shortcuts `-Nh` / `-Nm`. Lands
    /// on the first matching row via `FindFirstRowAtOrAfter` +
    /// `SelectSourceRow`, or status-hints if none qualifies.
    void PromptGotoTimestamp();

    /// Post-`exec` body of `PromptGotoLine` -- exposed so tests can
    /// drive the range check + filter-visibility hint without a
    /// modal dialog. Also called by `PromptGotoLine` after accept.
    void ExecuteGotoLine(const QString &input);

    /// Post-`exec` body of `PromptGotoTimestamp` -- exposed so
    /// tests can pin the clock (`now`) so relative shortcuts
    /// (`-1h`) are deterministic.
    void ExecuteGotoTimestamp(const QString &input, std::chrono::system_clock::time_point now);

    /// Pure parser for `PromptGotoTimestamp`. Static so unit tests
    /// can drive it without a `LogSessionView` instance. See
    /// `MainWindow::ParseGotoTimestampInput` docstring for the
    /// full input grammar (relative shortcut, per-column formats,
    /// ISO fallbacks, overflow rejection).
    [[nodiscard]] static std::optional<GotoTimestampParse> ParseGotoTimestampInput(
        const QString &input,
        const std::vector<std::string> &columnParseFormats,
        std::chrono::system_clock::time_point now
    );

    /// Last text typed into the Go to Timestamp dialog. Tests
    /// pin the session-boundary clear via this accessor; the
    /// shell's `LastGotoTimestampInputForTest` forwards here.
    [[nodiscard]] QString LastGotoTimestampInput() const noexcept
    {
        return mLastGotoTimestampInput;
    }

    /// Clear both sticky Goto inputs. Called from the session-
    /// switch path on `MainWindow` so a carry-over from a
    /// previous file does not leak into a fresh log.
    void ClearGotoStickyInputs() noexcept
    {
        mLastGotoLineInput.clear();
        mLastGotoTimestampInput.clear();
    }

    /// Test seam for the sticky-input latch (review-2 finding #4).
    /// The user-facing latch site inside `ExecuteGotoTimestamp`
    /// requires a populated model with a time column, which is a
    /// heavy fixture for a direct view test. This seam lets a
    /// unit test seed the latch, then exercise `ClearGotoStickyInputs`
    /// on non-empty state and observe the reset. Do NOT use in
    /// production paths.
    void SetLastGotoTimestampInputForTest(QString value) noexcept
    {
        mLastGotoTimestampInput = std::move(value);
    }

    // -----------------------------------------------------------------
    // Task 3.4 (continued): follow-newest + anchor navigation
    // migrated from `MainWindow`. QAction chrome
    // (`ui->actionFollowTail`, hotkeys) still lives on
    // `MainWindow` and forwards to these entry points.
    // -----------------------------------------------------------------

    /// Snap the viewport to the newest source row via the proxy
    /// chain. Used by the shell's "Jump to newest" pill click and
    /// the auto-follow-tail heuristic when a live-tail batch
    /// arrives. Migrated from `MainWindow::JumpToNewestRow`.
    void JumpToNewestRow();

    /// Move the selection to the next (or previous, when @p forward
    /// is false) anchor visible under the current filter/sort.
    /// Emits `statusMessageRequested` on the "no anchors set" /
    /// "no anchored rows visible" paths. Migrated from
    /// `MainWindow::JumpToAnchor`.
    void JumpToAnchor(bool forward);

    // -----------------------------------------------------------------
    // Task 3.7: tab-scoped progress presentation. `LogSession`
    // already owns the operation state / stop sources / atomics
    // (phase 2 tasks 2.8 / 2.9); the view now owns the visual
    // presentation. The shell continues to drive the modal
    // `QProgressDialog` for now (window-modal blocking is a shell
    // chrome decision); this API is the tab-scoped alternative
    // that phase 6 wires exclusively once tabs exist and the
    // whole-window blocking dialog stops making sense.
    // -----------------------------------------------------------------

    /// Show the per-tab progress strip with @p label and @p percent
    /// (0-100). Idempotent: safe to call on every progress tick.
    /// Emits `progressCancelRequested()` when the user clicks the
    /// embedded Cancel button. When @p percent < 0 the strip
    /// renders as indeterminate (busy indicator) rather than a
    /// fixed percentage.
    void ShowOperationProgress(const QString &label, int percent);

    /// Update just the label / percent on an already-visible
    /// progress strip. No-op when the strip is hidden -- callers
    /// should use `ShowOperationProgress` on the first tick.
    void UpdateOperationProgress(const QString &label, int percent);

    /// Hide the per-tab progress strip. Idempotent.
    void HideOperationProgress();

    /// True while the progress strip is currently shown; the shell
    /// uses this to decide whether to route a cancel-shortcut into
    /// the view's cancel button vs a shell-scoped action.
    [[nodiscard]] bool IsOperationProgressVisible() const noexcept;

signals:
    /// Emitted by `SelectSourceRow` when the requested source row
    /// is out of range or mapped away by the current filter/sort.
    /// The shell binds this to a `statusBar()->showMessage` so the
    /// user sees the "Row is not currently visible." feedback.
    void rowNotVisible();

    /// Emitted by `ScrollToProxyRow` before any scroll happens so
    /// the shell can drop the `actionFollowTail` check state
    /// (rail navigation is intentional browsing; a live-tail
    /// batch must not yank the viewport back to the tail).
    void followTailDisengageRequested();

    /// Emitted whenever the view needs to surface a transient
    /// status-bar message (e.g. Goto Line / Timestamp rejections).
    /// The shell wires this to `statusBar()->showMessage(msg,
    /// STATUS_BAR_MESSAGE_TIMEOUT_MS)`; a null shell listener
    /// simply drops the message.
    void statusMessageRequested(const QString &message);

    /// Emitted when the user clicks the Cancel button on the
    /// per-tab progress strip. The shell wires this into the
    /// active operation's cancel path (decompression / export
    /// stop-source, ...). No-op when the strip is hidden.
    void progressCancelRequested();

private:
    /// Shared ctor body used by both the themed and theme-less
    /// overloads. Instantiates the table view + overview rail
    /// widgets + rail model, places the table (only) in a
    /// zero-margin outer `QVBoxLayout`, hides the rail by default,
    /// and bare-parents the rail on `this` so
    /// `LogTableView::AttachOverviewRail` can reparent it into the
    /// table's reserved right viewport margin when the shell's
    /// visibility toggle turns the rail on.
    void Initialise(ThemeControl *theme);

    QPointer<LogSession> mSession;
    LogTableView *mTableView = nullptr;
    OverviewRailModel *mOverviewRailModel = nullptr;
    OverviewRailWidget *mOverviewRailWidget = nullptr;
    QVBoxLayout *mLayout = nullptr;

    /// Column index the level-cell delegate is currently installed
    /// on, or -1 when detached. Migrated from
    /// `MainWindow::mInstalledLevelDelegateColumn` (task 3.5) so
    /// the view can enforce the "detach before reinstall on a new
    /// column" invariant without a shell round-trip on every
    /// `modelReset` / `columnsInserted` / `columnsRemoved`.
    int mInstalledLevelDelegateColumn = -1;

    /// Last text typed into the Go to Timestamp dialog. Migrated
    /// from `MainWindow::mLastGotoTimestampInput` (task 3.6);
    /// cleared via `ClearGotoStickyInputs()` on the shell's
    /// session-switch path.
    QString mLastGotoTimestampInput;

    /// Last text typed into the Go to Line dialog. Migrated from
    /// `MainWindow::mLastGotoLineInput` (task 3.6). Re-validated
    /// against the current row count on open so a carry-over
    /// from a larger session no longer applies.
    QString mLastGotoLineInput;

    /// Task 3.7: per-tab progress strip. Container widget hosts a
    /// label + progress bar + cancel button in a horizontal
    /// layout, sits below the table view, and is hidden by
    /// default. Constructed on first `ShowOperationProgress` to
    /// keep the empty-view cost at zero for tabs that never run
    /// a background operation.
    QWidget *mProgressStrip = nullptr;
    QLabel *mProgressLabel = nullptr;
    QProgressBar *mProgressBar = nullptr;
    QPushButton *mProgressCancelButton = nullptr;

    /// Lazy-construct the progress strip widgets. Called from
    /// `ShowOperationProgress`; subsequent calls short-circuit.
    void EnsureProgressStrip();
};
