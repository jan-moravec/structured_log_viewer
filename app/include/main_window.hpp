#pragma once

#include "anchor_manager.hpp"
#include "anchors_dock.hpp"
#include "find_dock.hpp"
#include "find_record_widget.hpp"
#include "histogram_dock.hpp"
#include "log_filter_model.hpp"
#include "log_model.hpp"
#include "log_session.hpp"
#include "log_session_view.hpp"
#include "log_table_view.hpp"
#include "overview_rail_model.hpp"
#include "overview_rail_widget.hpp"
#include "parse_errors_dock.hpp"
#include "preferences_editor.hpp"
#include "record_detail_dock.hpp"
#include "record_detail_window.hpp"
#include "row_order_proxy_model.hpp"
#include "scoped_connections.hpp"
#include "session_bind_context.hpp"

#include <loglib/internal/decompressing_byte_source.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/stop_token.hpp>
#include <loglib/theme.hpp>

// Forward-declared so this header does not pull in `<date/tz.h>`
// just for the Goto Timestamp helpers' zone argument.
namespace date
{
class time_zone;
} // namespace date

// `loglib::EnumDictionary` is referenced via `ResolveEnumDictionary` below;
// the full type comes in transitively through `log_filter_model.hpp`.

namespace loglib
{
class BytesProducer;
} // namespace loglib

#include <QAction>
#include <QApplication>
#include <QAtomicInteger>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QHash>
#include <QLabel>
#include <QList>
#include <QMainWindow>
#include <QMimeData>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

QT_BEGIN_NAMESPACE
namespace Ui
{
class MainWindow;
}
class QMenu;
class QProgressDialog;
template <typename T> class QFutureWatcher;
QT_END_NAMESPACE

class SessionHistoryManager;
class ThemeControl;
class RegexTemplateRegistry;
class RegexTemplatesEditor;
class HighlightRuleSet;
class HighlightRulesEditor;

namespace slv::exports
{
struct ExportPlan;
} // namespace slv::exports

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    /// Alias of the authoritative session-mode enum on `LogSession`
    /// (task 2.5). Declared here at the top of the class so
    /// every downstream signature can spell it as `SessionMode`
    /// instead of `LogSession::Mode`.
    using SessionMode = LogSession::Mode;

    /// Selects how `StartStreamingOpenQueue` interacts with the
    /// current state. `Append` queues new files onto the active
    /// static session without clobbering its filters / sort / rows.
    /// `Replace` resets the model, clears filters, and drops the
    /// source descriptor first. Live-tail / network sessions are
    /// single-source and always behave as `Replace`.
    enum class OpenMode
    {
        Append,
        Replace,
    };

    /// Outcome of `DispatchMixedOpenInput`. Lets callers attach
    /// entry-point-specific tails (e.g. the CLI `AppliedConfigOnly`
    /// status-bar hint) based on which branch the dispatcher took.
    enum class MixedInputDispatch
    {
        /// No configs in the input -- streamed via
        /// `StartStreamingOpenQueue` in the caller's `OpenMode`.
        QueuedLogsOnly,
        /// One config, no logs -- applied via `TryLoadAsConfiguration`
        /// (no model reset; existing rows survive).
        AppliedConfigOnly,
        /// One config + N logs -- applied via `DoLoadConfiguration`
        /// (full reset), then logs streamed in `Append` mode so the
        /// freshly-loaded columns / filters / sort apply.
        AppliedConfigThenLogs,
        /// Two or more configs -- rejected with a warning dialog and
        /// no state mutated.
        RejectedMultiConfig,
    };

    /// Full result of `DispatchMixedOpenInput`. `appliedConfigPath`
    /// is non-empty iff @outcome is `AppliedConfigOnly` or
    /// `AppliedConfigThenLogs`. Threading the chosen path back to
    /// the caller lets user-facing status messages name the actual
    /// configuration argument (not `files.front()`, which silently
    /// lies when the config is not the first positional).
    struct MixedInputResult
    {
        MixedInputDispatch outcome = MixedInputDispatch::QueuedLogsOnly;
        QString appliedConfigPath;
    };

    /// No-history, no-theme constructor: auto-save / Recent
    /// Sessions / restore-on-launch are all no-ops, and the table
    /// renders without per-level styling. Used by the legacy
    /// `MainWindow mainWindow;` test sites that don't exercise
    /// the theme system; pair the test fixture with a real
    /// `ThemeControl` via the themed overload for theme-aware
    /// assertions.
    MainWindow(QWidget *parent = nullptr);

    /// Themed, no-history constructor for test fixtures and
    /// ad-hoc instances that need a live theme but don't care
    /// about session history.
    MainWindow(ThemeControl *theme, QWidget *parent = nullptr);

    /// Production constructor. The theme controller, history
    /// manager and regex-template registry are owned by `main()`;
    /// the window keeps non-owning pointers and writes snapshots
    /// through the history manager on streaming completion /
    /// close. Any of `theme` / `regexTemplateRegistry` may be
    /// nullptr in tests; theme code paths fall back to defaults,
    /// and the network-stream dialog falls back to the library's
    /// built-in template catalog.
    ///
    /// No separate `(theme, history, parent)` overload exists on
    /// purpose: it would make `MainWindow(theme, history, nullptr)`
    /// test calls ambiguous with this one. Tests that don't need a
    /// registry keep their 3-arg shape and resolve here with
    /// `parent` defaulted.
    MainWindow(
        ThemeControl *theme,
        SessionHistoryManager *historyManager,
        RegexTemplateRegistry *regexTemplateRegistry,
        QWidget *parent = nullptr
    );

    ~MainWindow();

    /// Locate the staged `tzdata/` directory and initialise loglib's
    /// timezone database from it. Idempotent.
    ///
    /// Must be called before any timestamp-formatting code path.
    /// `main()` calls this before constructing the primary window
    /// and before the restore-on-launch flow; the test fixture
    /// mirrors the call in `initTestCase`. Without this ordering
    /// the first `loglib::CurrentZone()` (triggered by loading a
    /// session with a time-range filter) probes the date library's
    /// platform default path (on Windows: `<profile>/Downloads/tzdata`)
    /// and fails with a misleading "Error Parsing Configuration".
    ///
    /// Returns true on success. On failure logs a `qCritical`
    /// diagnostic and returns false; `main()` propagates that as a
    /// non-zero exit code.
    [[nodiscard]] static bool InitializeTimezoneDatabase();

    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

    /// Restore the auto-saved session at @p jsonPath. Same logic as
    /// the Recent Sessions reopen path, but starts from a JSON path
    /// so it can run before any menu wiring (used by `main()`'s
    /// restore-on-launch flow).
    ///
    /// `mAutoSaveUuid` is pinned only when the stem parses as a
    /// `QUuid` AND @p jsonPath lives in `mHistoryManager->SessionsDir()`.
    /// For external / non-uuid JSONs the configuration loads but the
    /// pin is skipped: pinning would let the next AutoSave write a
    /// managed copy under that stem and silently fork the user's
    /// original file. External JSONs stay read-only in place; the
    /// next save mints a fresh uuid.
    void RestoreLastSessionFromPath(const QString &jsonPath);

    /// Open CLI-provided file paths. Behaves like `OpenFiles` but
    /// bypasses the dialog; used by `main()` after parsing argv and
    /// by the single-instance forward handler. Always Append mode
    /// so pre-loaded configuration filters survive into the new
    /// session.
    void OpenFilesForCli(const QStringList &files);

    /// Set this window's CLI rotation-history opt-out. It overrides
    /// global and session preferences until the user toggles the
    /// corresponding Settings action.
    void SetRotationHistoryLaunchOverride(bool disable);

    /// Test-only reader for this window's CLI opt-out.
    [[nodiscard]] bool RotationHistoryLaunchOverrideForTest() const noexcept
    {
        return mSession->DisableRotationHistoryOverride();
    }

    /// Test-only entry to the rotation-history Settings handler.
    void SimulateRotationHistoryMenuToggleForTest(bool enabled)
    {
        OnRotationHistoryPrefToggled(enabled);
    }

    /// Test-only reader for the scoped-connection bag population.
    /// Backs the structural pin
    /// (`TestScopedConnectionBagSizeMatchesCtorPopulation`) that
    /// catches the class of regressions where a ctor `connect(...)`
    /// accidentally drops its `mSessionConnections +=` prefix.
    /// See the header docstring on `mSessionConnections` for the
    /// bag's phase-4 contract.
    [[nodiscard]] std::size_t SessionConnectionCountForTest() const noexcept
    {
        return mSessionConnections.Size();
    }

    /// Open a live-tail session over the process's standard input.
    /// The CLI parses `-` / `--stdin` in argv and routes here via
    /// `main()`. Session shape mirrors `OpenNetworkStream` (live-
    /// tail, `Kind::Stdin`, excluded from Recent Sessions): stdin is
    /// one-shot per process, so there's nothing to reopen on restart.
    void OpenStdinStream();

    /// Shared implementation for `OpenStdinStream` and its test
    /// seam. Takes ownership of the producer and its pre-read bytes.
    void OpenStdinStreamFromProducer(std::unique_ptr<loglib::BytesProducer> producer, std::string peek);

    /// The auto-save uuid pinned to this window, or empty if none.
    /// Used by `main()`'s `aboutToQuit` snapshot.
    [[nodiscard]] QString ActiveSessionUuid() const noexcept
    {
        return mSession->AutoSaveUuid();
    }

    /// The currently-active `LogSession` for this window (task 4.1).
    /// Phase 3 hosts exactly one session; Phase 6 grows this into a
    /// lookup that follows the active tab. Never returns null in
    /// production because the ctor constructs `mSession` before
    /// any callable path runs and never zeroes it. Note (post
    /// review-5): `mSession` is a raw `LogSession *`, not a
    /// `QPointer`, so it **dangles** rather than becoming null
    /// after `~LogSession()`. Long-lived callbacks that stash
    /// the pointer must therefore capture a
    /// `QPointer<LogSession>` snapshot (not the raw pointer this
    /// method returns) and compare its `data()` to the current
    /// `mSession` inside the callback body -- the `QPointer`
    /// clears on Qt-destroyed and is the only safe way to detect
    /// destruction here.
    ///
    /// **Trigger-time-resolution rule (PRD FR-51)**: every action
    /// slot / dynamic menu builder / status projection must resolve
    /// the session AT trigger time, not at wiring time. Reading
    /// `mSession` inside a slot body satisfies this because the
    /// ctor keeps `mSession` in lockstep with the active tab (and
    /// phase 6's `SetActiveSessionAliases` will do the same across
    /// tab switches). **Do not** store the raw `LogSession *`
    /// returned here in a lambda capture or long-lived data member
    /// -- that captures the value at wiring time and phase 6's tab
    /// switch (or a session teardown) will leave the capture
    /// pointing at a stale / destroyed session. If a queued or
    /// async callback legitimately needs its ORIGIN session (PRD
    /// FR-44), capture a `QPointer<LogSession>` snapshot at post
    /// time and compare it to `activeSession()` inside the
    /// callback body -- the callback must silently no-op if the
    /// origin was destroyed or is no longer active.
    [[nodiscard]] LogSession *activeSession() const noexcept
    {
        return mSession;
    }

    /// The currently-active `LogSessionView` for this window (task
    /// 4.1). Same phase-3-single-view / phase-6-follows-active-tab
    /// story as `activeSession()`. Never returns null in
    /// production; the ctor constructs the view before the shell
    /// wires any action into it. Same trigger-time-resolution rule
    /// applies: read at slot-body scope, do not capture.
    [[nodiscard]] LogSessionView *activeSessionView() const noexcept
    {
        return mSessionView;
    }

    /// Build a `SessionBindContext` snapshot of the currently-active
    /// session + view (task 5.1). Convenience wrapper around
    /// `SessionBindContext::FromSessionAndView(activeSession(),
    /// activeSessionView(), mTheme)`; returns
    /// `SessionBindContext::MakeUnbound()` if either accessor is
    /// null (typical during a mid-teardown observer read).
    ///
    /// Consumers: (a) phase-6 tab-switch orchestrator, which
    /// re-binds every shared dock against this context; (b)
    /// tests that need to hand a dock a live bind context
    /// without reconstructing MainWindow's internals; (c) the
    /// ctor's initial `RebindSharedDocks()` call.
    [[nodiscard]] SessionBindContext activeSessionBindContext() const;

    /// Every `LogSession` this window is hosting, in tab order
    /// (task 4.1 / 4.6 / 4.8). Phase 3 hosts exactly one; Phase 6
    /// grows the vector to the workspace's tab list. Callers must
    /// treat the returned pointers as non-owning views into
    /// window-scoped storage (do NOT persist across ctor / dtor).
    ///
    /// Used by:
    ///   * `AggregateWindowModified()` (task 4.6) -- window is
    ///     dirty iff any hosted session is dirty.
    ///   * `BroadcastGlobalPreference*()` (task 4.7) -- a
    ///     process-global preference change fans to every hosted
    ///     session, not just the active one.
    ///   * `closeEvent` (task 4.8) -- each hosted session runs its
    ///     own `PreCheckClose()` / auto-save / drain before the
    ///     window commits to closing.
    [[nodiscard]] std::vector<LogSession *> hostedSessions() const;

    /// Clear the scoped-connection bag (task 4.2). Every
    /// subscription installed via `mSessionConnections +=
    /// connect(...)` in the ctor is disconnected atomically.
    /// Idempotent: safe to call on an already-empty bag and safe to
    /// call from `~MainWindow` before the model / session / view
    /// members go away.
    ///
    /// Leaves `mSession` / `mSessionView` / model-quintet aliases
    /// untouched -- a caller that wants to null the active pointers
    /// should do so explicitly after this call.
    ///
    /// **Phase 4 scope**: production callers are limited to the
    /// destructor. `MainWindow` does not yet provide a paired
    /// `BindActiveSession` reinstall path -- the ~50 shell-side
    /// subscriptions are wired inline in the ctor. Calling this
    /// method from any other production path would leave the
    /// window functionally deaf until the ctor runs again (i.e.
    /// never). The `ForTest` suffix on the exposed entry
    /// documents that constraint: production code must not reach
    /// through it. Phase 6 will add a `RebindActiveSession(session,
    /// view)` driver that both clears and re-populates the bag on
    /// every tab switch; this raw teardown-only entry will move
    /// to `private` at that point.
    void UnbindActiveSessionForTest() noexcept;

    /// Deterministic shared-dock rebind driver (task 5.2).
    ///
    /// Routes @p context to every shared dock and dialog through
    /// its `Bind(SessionBindContext)` slot in a fixed order:
    ///
    ///   1. Docks whose state is authoritative on the session (find,
    ///      parse errors, histogram, record detail) rebind first so
    ///      their save-outgoing / restore-incoming step observes the
    ///      full model quintet still bound to the outgoing session's
    ///      QObjects; then
    ///   2. Anchors dock rebinds (anchor model swap needs the session
    ///      already re-aliased); then
    ///   3. Session-scoped dialogs (columns manager, configuration
    ///      diagnostics, highlight rules editor) rebind or close per
    ///      their tab-change policy (tasks 5.8 / 5.9).
    ///
    /// A context whose `IsBound()` is false (typically
    /// `SessionBindContext::MakeUnbound()`) drives every dock into
    /// its "no active session" state -- persistent indexes cleared,
    /// debounce timers cancelled, session-specific chrome hidden.
    ///
    /// **Phase 5 scope**: the ctor is the only production caller
    /// today. Per-dock `Bind(SessionBindContext)` slots land
    /// subtask-by-subtask (5.3 = FindDock, 5.4 = ParseErrorsDock,
    /// 5.5 = AnchorsDock, 5.6 = HistogramDock, 5.7 = RecordDetailDock,
    /// 5.8 = ColumnsManagerDialog + ConfigurationDiagnosticsDialog,
    /// 5.9 = HighlightRulesEditor); docks that have not yet been
    /// refactored are skipped here without warning until their
    /// subtask lands. Phase 6 promotes this into the primary tab-
    /// switch entry, driven from the workspace's active-tab
    /// selection.
    void RebindSharedDocks(const SessionBindContext &context);

    /// Aggregate the modified-window state from every hosted
    /// session (task 4.6). Called from the `filtersDirtyChanged`
    /// slot and from `UpdateWindowTitle`; single-session windows
    /// resolve to the active session's `IsFiltersDirty()` value,
    /// but the same code path scales to a multi-tab window
    /// without touching the callers.
    void AggregateWindowModified();

    /// Broadcast a process-global preference change to every
    /// hosted session (task 4.7). The bool payload is the new
    /// value of the `ui/autoDetectRotatedHistory` `QSettings`
    /// key; per-session filtering happens inside
    /// `LogSession::ShouldAutoDetectRotationHistory` (the CLI
    /// per-window opt-out still latches). Called from the
    /// Settings menu action and from anywhere else that changes
    /// the preference at runtime.
    void BroadcastRotationHistoryPreference(bool enabled);

    /// Like `ActiveSessionUuid`, but returns empty when the current
    /// session cannot be fan-restored on next launch (no source,
    /// network stream, ...). `main()`'s `aboutToQuit` handler uses
    /// this to avoid publishing non-restorable uuids into
    /// `openWindowsAtQuit` (which would otherwise loop the user on
    /// the "Network Stream Session" info popup every launch).
    [[nodiscard]] QString RestorableActiveSessionUuid() const noexcept;

    /// Mirror runtime session state into the configuration manager,
    /// then `WriteSnapshot` through the injected history manager.
    /// Reuses `mAutoSaveUuid` so a single window updates one recents
    /// entry across its lifetime. No-op when the manager is null or
    /// there is no source descriptor.
    ///
    /// When @p publishOpenWindow is true (the default), adds
    /// `mAutoSaveUuid` to `openWindowsAtQuit` so a crash between
    /// AutoSave and `closeEvent` still restores this window. The
    /// `closeEvent` flush passes false because it immediately
    /// removes the uuid again.
    ///
    /// Public so `main()`'s `aboutToQuit` handler can flush every
    /// live window before exit.
    void AutoSaveSessionSnapshot(bool publishOpenWindow = true);

    void UpdateUi();

    /// Single sync point for newest-first display: picks the right
    /// `StreamingControl` flag for the active session mode and
    /// propagates it to the proxy, table view, and row colours.
    /// Idempotent.
    void ApplyDisplayOrder();

    /// Test-only direct accessor for the live filter proxy. Tests
    /// inspect the filtered row count and column-rank state through
    /// it; production wires the proxy into `mTableView` directly.
    [[nodiscard]] LogFilterModel *FilterModel() const
    {
        return mSortFilterProxyModel;
    }

    /// Toggle column visibility. Updates `Column::visible` and the
    /// header. No-op for an out-of-range index. Public for tests and
    /// the View menu.
    void SetColumnVisible(int logicalIndex, bool visible);

    /// Open the per-column editor dialog modally on @p columnIndex.
    /// Reached from the header right-click menu, the diagnostics
    /// dialog (row double-click), and the columns manager's
    /// Edit\u2026 button. No-op for out-of-range indices.
    void EditColumn(int columnIndex);

    /// Show the modeless `ColumnsManagerDialog` (constructed lazily).
    /// A second call raises the existing instance.
    void ShowColumnsManager();

    /// Show + raise the Record Details dock and pin it to @p proxyIndex
    /// (mapped to a source row internally). Invalid index: no-op.
    void ShowRecordDetailsForProxyIndex(const QModelIndex &proxyIndex);

    /// Sync the dock to the table's current selection. No-op when the
    /// dock is hidden (avoids work on an invisible widget).
    void UpdateRecordDetailsFromSelection();

    /// Open a standalone `RecordDetailWindow` snapshot of source row
    /// @p sourceRow. Out-of-range rows are a no-op.
    void OpenRecordDetailWindow(int sourceRow);

    /// Push every `Column::visible` flag to the header. Idempotent;
    /// run after a load or reorder.
    void ApplyColumnVisibility();

    /// Install (or detach) the icon-pill delegate on the current
    /// first `Type::Level` column. Idempotent; detaches from the
    /// previous column when the level column has moved, and
    /// detaches entirely when no level column exists. Safe on the
    /// streaming hot path (at most two `setItemDelegateForColumn`
    /// calls).
    ///
    /// View column index == source column index here because both
    /// proxies (`LogFilterModel`, `RowOrderProxyModel`) pass
    /// `columnCount` through 1:1. A future column-reordering proxy
    /// would have to remap.
    void ApplyLevelCellDelegate();

    /// Restore the header so visual == logical for every section.
    /// Suppresses re-entry into `OnHeaderSectionMoved` while doing
    /// so. Idempotent.
    void ResetHeaderToIdentity();

    /// Result of `BuildHeaderContextMenu`. Caller owns `menu`.
    struct HeaderContextMenu
    {
        QMenu *menu = nullptr;
    };

    /// Build the right-click header menu for @p logicalColumn.
    /// Caller owns `result.menu`. `result.menu` is null when
    /// @p logicalColumn is out of range.
    [[nodiscard]] HeaderContextMenu BuildHeaderContextMenu(int logicalColumn, QWidget *parent = nullptr);

    /// Build the row right-click menu for source-model row @p sourceRow.
    /// Always includes the "Anchor" sub-menu; adds "Show only newer/
    /// older logs" actions when the row has a non-`monostate` timestamp
    /// in the first `Type::Time` column.
    ///
    /// Returns null when the model is empty or @p sourceRow is out of
    /// range. Caller owns the result; parented to @p parent (or
    /// `mTableView` if null).
    [[nodiscard]] QMenu *BuildRowContextMenu(int sourceRow, QWidget *parent = nullptr);

    /// Live simple-mode filter leaves keyed by UUID (UI-only
    /// identifier for menu wiring). Persisted state lives on
    /// `LogConfiguration::expression`. Backed by `mSession` after
    /// task 2.4; the accessor shape is preserved for tests.
    [[nodiscard]] const std::unordered_map<std::string, loglib::LeafRule> &Filters() const;

    /// Owned `LogModel`; non-null after construction.
    [[nodiscard]] LogModel *Model() const
    {
        return mModel;
    }

    /// Source-model row indices that `File -> Export Filtered
    /// Rows...` would walk, in the same top-to-bottom order the
    /// user sees. Respects every proxy layer (`LogFilterModel`
    /// sort, `RowOrderProxyModel` newest-first flip); when
    /// @p selectionOnly is true, restricts to the current
    /// selection. Public so tests can pin ordering without
    /// going through the modal `ExportDialog`; production code
    /// should use `ExportFilteredRows` instead.
    [[nodiscard]] std::vector<int> CollectExportSourceRows(bool selectionOnly) const;

    /// Owned `AnchorManager`; non-null after construction.
    [[nodiscard]] AnchorManager *Anchors() const noexcept
    {
        return mAnchors;
    }

    /// Owned `HighlightRuleSet`; non-null after construction.
    /// Exposed for test inspection of the rebind signal path.
    [[nodiscard]] HighlightRuleSet *Highlights() const noexcept
    {
        return mHighlights;
    }

    /// Select the next (forward=true) or previous anchored row in
    /// visible (proxy) order, honouring sort + filter + newest-first
    /// orientation. Wraps at the visible bounds. Surfaces a status-bar
    /// note when no anchored row is visible. Wired to F2 / Shift+F2.
    void JumpToAnchor(bool forward);

    /// Open a one-line note editor for the anchor at @p key. No-op
    /// (with a status-bar hint) when the anchor is gone -- e.g. a
    /// queued eviction or `Ctrl+0` between menu build and click.
    /// Committed text is sanitised and forwarded to
    /// `AnchorManager::SetAnchorNote`.
    ///
    /// Row context menu uses this path so it captures the anchor
    /// *identity* (not a row index), preventing a mid-menu FIFO
    /// eviction from redirecting the edit to a different row.
    void EditAnchorNoteForKey(const AnchorManager::Key &key);

    /// Row-index wrapper around `EditAnchorNoteForKey`. Used by F4
    /// and tests; the row-menu path captures a key directly to
    /// avoid the eviction race.
    void EditAnchorNoteForRow(int sourceRow);

    /// F4 handler: resolves the focused proxy row to a source row
    /// and calls `EditAnchorNoteForRow`. Shows a status-bar hint
    /// when no row is focused or the row isn't anchored.
    void EditAnchorNoteOnCurrentRow();

    /// Scroll to source row @p sourceRow and make it the sole
    /// selection. No-op on a negative row, unready model, or a row
    /// that is currently filtered out (the latter shows a status bar
    /// note). Used by the Anchors dock for jump targets.
    void SelectSourceRow(int sourceRow);

    /// Pop the "Go to Line..." modal (`Ctrl+G`). Line numbers are
    /// 1-based over the source model as it currently is; line 1 is
    /// always the earliest retained row (streaming FIFO eviction
    /// may have dropped older rows, so numbers need not match the
    /// source file's numbering). Newest-first display reversal only
    /// affects rendering, not the number the user types. Rejects
    /// and status-bar-hints on any error; a valid row hands off to
    /// `SelectSourceRow`. Post-dialog work lives in
    /// `ExecuteGotoLine` so tests can drive it without a modal.
    void GotoLine();

    /// Pop the "Go to Timestamp..." modal (`Ctrl+Shift+G`). Accepts
    /// the current time column's `parseFormats`, two ISO fallbacks
    /// (`%FT%T`, `%F %T`), and the relative shortcuts `-Nh` /
    /// `-Nm`. Naive inputs (no `%z` / `%Z` in the winning format)
    /// are shifted from the table's display time zone
    /// (`loglib::CurrentZone()`) to UTC before the search. Lands
    /// on the first matching row via `FindFirstRowAtOrAfter` +
    /// `SelectSourceRow`, or status-bar-hints if none qualifies.
    void GotoTimestamp();

    /// Result of `ParseGotoTimestampInput`. `micros` is epoch
    /// microseconds; `isNaive` is true when the winning format had
    /// no zone specifier (`%z` / `%Ez` / `%Z`) and the caller must
    /// therefore shift `micros` through the display TZ before
    /// comparing against stored (UTC-normalised) timestamps. The
    /// relative-shortcut path always returns `isNaive == false`
    /// since it derives from `system_clock::now()` (already UTC).
    struct GotoTimestampParse
    {
        int64_t micros = 0;
        bool isNaive = false;
    };

    /// Pure parser for `GotoTimestamp`. Tries, in order: the
    /// relative shortcut `^[+-]?\s*(\d+)\s*([hm])\s*$` (case-
    /// insensitive; `+` / no-sign also mean "N units ago", matching
    /// lnav / less); each entry of @p columnParseFormats via
    /// `loglib::TryParseTimestamp`; the ISO fallbacks `%FT%T` and
    /// `%F %T`. Returns `std::nullopt` on no match or on relative-
    /// shortcut overflow (silent wrap would jump the user forward,
    /// which is worse than a hint). Static so unit tests can drive
    /// it without a `MainWindow` instance.
    [[nodiscard]] static std::optional<GotoTimestampParse> ParseGotoTimestampInput(
        const QString &input,
        const std::vector<std::string> &columnParseFormats,
        std::chrono::system_clock::time_point now
    );

    /// First row whose timestamp on @p timeCol is `>= targetMicros`
    /// AND is currently visible through the outer proxy (respects
    /// the active row filter). Returns the source-model row index
    /// or `-1` when nothing qualifies.
    ///
    /// Three branches, picked from (user-sort, monotonicity):
    ///
    /// * **Fast path** -- no user sort, `TimestampsAreMonotonic()`
    ///   true. Binary-search source rows (O(log N)); on a filter
    ///   miss, walk the outer proxy for the smallest visible row
    ///   with a valid ts `>= lo` (O(N_visible) worst case, not
    ///   O(N_source)). Missing timestamps are skipped, not
    ///   `-inf`-treated.
    /// * **Non-monotonic path** -- no user sort, monotonicity
    ///   false (multi-file `Append`, rotation, clock skew). Walk
    ///   the outer proxy and pick the visible row with the
    ///   smallest ts satisfying `>= target` (chronologically
    ///   earliest match).
    /// * **User-sort path** -- header sort active. Linear scan in
    ///   display order and return the first proxy row whose source
    ///   ts qualifies, so "first" honours the user's sort.
    ///
    /// Public so tests can drive all three branches directly.
    [[nodiscard]] int FindFirstRowAtOrAfter(int timeCol, int64_t targetMicros) const;

    /// Jump the table to the first row in histogram bucket
    /// @p bucketIndex. Wired to `HistogramDock::bucketClicked`.
    void JumpToFirstRowInBucket(std::size_t bucketIndex);

    /// Scroll the table so proxy row @p proxyRow is centred.
    /// Wired to `OverviewRailWidget::proxyRowClicked`; no-op when
    /// the row is out of range.
    ///
    /// @p replaceSelection: `true` on a fresh rail click (clears
    /// the existing selection and selects just @p proxyRow);
    /// `false` during a drag scrub (leaves selection alone so a
    /// multi-row selection survives a rail scroll).
    ///
    /// Always disengages Follow newest: rail navigation is
    /// intentional browsing, and `scrollTo` alone wouldn't fire
    /// `userScrolledAwayFromTail`.
    void ScrollToProxyRow(int proxyRow, bool replaceSelection = true);

    /// Attach / detach `mOverviewRailWidget` on the table view,
    /// persist visibility to `QSettings("ui/showOverviewRail")`,
    /// and mirror the state onto `mActionToggleOverviewRail`.
    /// Idempotent so the load-time seed and the user toggle share
    /// one code path.
    void SetOverviewRailVisible(bool visible);

    /// Install a `Type::Time` filter on
    /// `[fromEpochMicros, toEpochMicros]` for the histogram's time
    /// column. Wired to `HistogramDock::timeRangeSelected`; no-op
    /// when the log has no time column.
    void AddTimeRangeFilterFromHistogram(qint64 fromEpochMicros, qint64 toEpochMicros);

#ifdef LOGAPP_BUILD_TESTING
    /// Test-only session-mode override so display-order tests can
    /// exercise the `Static` branch without a real open flow.
    enum class TestSessionMode
    {
        Idle,
        Static,
        LiveTail,
    };
    void SetSessionModeForTest(TestSessionMode mode);

    /// Test-only reader for the current session mode.
    [[nodiscard]] TestSessionMode SessionModeForTest() const noexcept;

    /// Test-only entry to the `TryLoadAsConfiguration` path
    /// (production gates it behind `QFileDialog`).
    bool TryLoadAsConfigurationForTest(const QString &file);

    /// Test-only entry to `SetConfigurationUiEnabled` so the
    /// column-management gate can be exercised without a real
    /// streaming session.
    void SetConfigurationUiEnabledForTest(bool enabled);

    /// Test-only entries to `SaveConfiguration` / `LoadConfiguration`
    /// that bypass the file dialog. `scope` defaults to `Full` so
    /// existing tests (written against the old single-action save)
    /// keep passing; pass `SaveScope::ColumnsOnly` for the
    /// "Save Configuration\u2026" path.
    void SaveConfigurationToPathForTest(const QString &path, loglib::SaveScope scope = loglib::SaveScope::Full);
    void LoadConfigurationFromPathForTest(const QString &path);

    /// When true, `ShowDroppedFiltersDialog` skips the modal and
    /// only updates the test counter (modals block any headless
    /// QtTest thread). Default false.
    void SetSuppressDialogsForTest(bool suppress);

    /// Filters dropped on the most recent
    /// `LoadConfigurationFromPathForTest` call. Reset on each load.
    [[nodiscard]] int LastDroppedFilterCountForTest() const;

    /// Test-only setter for `mCurrentSource`. Lets fixture-driven
    /// tests assert the descriptor round-trips through Save Session
    /// without running a real open path.
    void SetCurrentSourceForTest(std::optional<loglib::LogConfiguration::Source> source);

    /// Test-only read accessor for `mCurrentSource`; lets tests
    /// inspect the descriptor after an open or load.
    [[nodiscard]] const std::optional<loglib::LogConfiguration::Source> &CurrentSourceForTest() const noexcept;

    /// Test-only accessor for the source label used by
    /// `UpdateStreamingStatus`.
    [[nodiscard]] const QString &StreamingFileNameForTest() const noexcept
    {
        return mSession->StreamingFileName();
    }

    /// Test-only entry to `ShowRowContextMenu` so tests can pin
    /// right-click selection-adoption rules without a real mouse
    /// event. Callers should close any popup that opens.
    void ShowRowContextMenuForTest(const QPoint &pos)
    {
        ShowRowContextMenu(pos);
    }

    /// Test-only entry to the queued static-files open path,
    /// bypassing the file dialog and modifier sniff.
    void OpenFilesForTest(const QStringList &files, OpenMode mode);

    /// Test-only entry to the mixed-input dispatcher. Returns the
    /// branch the dispatcher took so tests can assert on the shape
    /// without scraping the status bar.
    MixedInputDispatch OpenMixedFilesForTest(const QStringList &files, OpenMode logMode);

    /// Drive the post-dialog body of `OpenLogStream` with @p filePath.
    /// Lets tests exercise the live-tail open path without a real
    /// modal `QFileDialog`.
    void OpenLogStreamForTest(const QString &filePath);

    /// Test-only check for a pending historical-prefix promotion.
    [[nodiscard]] bool HasPendingLiveTailForTest() const noexcept
    {
        return mSession->HasPendingLiveTailPromotion();
    }

    /// Test-only setter for pending live-tail promotion state.
    void SeedPendingLiveTailForTest(const QString &primary, size_t retention)
    {
        mSession->SetPendingLiveTailPromotion(primary, retention);
    }

    /// Test-only entry to the no-prefix live-tail rescue path.
    void TriggerRescueLiveTailForTest(const QString &primary, size_t retention);

    /// Test-only entry to `UndoRotationExpansion`.
    void UndoRotationExpansionForTest()
    {
        UndoRotationExpansion();
    }

    /// Original inputs captured for the most recent expansion.
    [[nodiscard]] const QStringList &LastRotationExpansionOriginalInputsForTest() const noexcept
    {
        return mSession->LastRotationExpansionOriginalInputs();
    }

    /// Seed expansion-undo state. Live-tail state reopens through
    /// `OpenLogStreamFromPath`; other state uses the static queue.
    void SeedLastRotationExpansionForTest(const QStringList &originalInputs, bool wasLiveTail)
    {
        mSession->SetLastRotationExpansion(originalInputs, wasLiveTail);
        if (mActionUndoRotationExpansion != nullptr)
        {
            mActionUndoRotationExpansion->setEnabled(!originalInputs.isEmpty());
        }
    }

    /// Test-only reader for the expansion-undo action state.
    [[nodiscard]] bool IsUndoRotationExpansionEnabledForTest() const noexcept
    {
        return mActionUndoRotationExpansion != nullptr && mActionUndoRotationExpansion->isEnabled();
    }

    /// Test seam; forwards a producer and pre-read bytes to
    /// `OpenStdinStreamFromProducer`.
    void OpenStdinStreamForTest(std::unique_ptr<loglib::BytesProducer> producer, std::string peek);

    /// Test-only forwarder to `NewSession`.
    void NewSessionForTest()
    {
        NewSession();
    }

    /// Test-only forwarder to the `OpenRecentSession` private slot.
    void OpenRecentSessionForTest(const QString &uuid)
    {
        OpenRecentSession(uuid);
    }

    /// Test seam for the `JumpToNewestRow` private helper; lets
    /// the filtered-newest-row fallback be exercised without
    /// synthesising a real pill click.
    void JumpToNewestRowForTest()
    {
        JumpToNewestRow();
    }

    /// Test-only accessor for the in-flight decompression flag. Lets
    /// timing-sensitive tests confirm a worker is armed without
    /// racing on wall-clock sleeps.
    [[nodiscard]] bool IsDecompressionInFlightForTest() const noexcept
    {
        return mSession->IsDecompressionInFlight();
    }
    /// Return whether a pending bundle may apply embedded configuration.
    [[nodiscard]] bool AppliesEmbeddedBundleConfigForNextOpenForTest() const noexcept
    {
        return mSession->ShouldApplyEmbeddedBundleConfig();
    }
    /// Simulate superseding a pending bundle decompression.
    void SimulateSupersededBundleDecompressionForTest()
    {
        mSession->SetApplyEmbeddedBundleConfigForPath(QStringLiteral("simulated-bundle.slvbundle"));
        mSession->SetDecompressionInFlight(true);
        CancelInFlightDecompression();
    }

    /// Test-only cancel injection: raises the same stop request
    /// `QProgressDialog::canceled` sends. Needed because the dialog
    /// is suppressed under `SetSuppressDialogsForTest`, making the
    /// production cancel wiring unreachable from tests. Callers must
    /// pump the event loop to drive the finished slot. No-op when
    /// no decompression is in flight.
    void RequestDecompressionCancelForTest()
    {
        if (mSession->IsDecompressionInFlight())
        {
            mSession->MutableDecompressionStopSource().request_stop();
        }
    }

    /// Export a bundle without showing the dialog, using production
    /// preflight checks and default zstd settings.
    void ExportSessionBundleToPathForTest(const QString &destination);

    /// Return whether an export worker is active.
    [[nodiscard]] bool IsExportInFlightForTest() const noexcept
    {
        return mSession->IsExportInFlight();
    }

    /// Test seam replaying the anchor-note commit path without a
    /// modal `QInputDialog`. Applies the same row-must-be-anchored
    /// guard, then forwards @p note to `SetAnchorNote` (which
    /// sanitises before storage). Returns true iff the row was
    /// anchored (identical-note no-ops still return true). Returns
    /// false on an unanchored row -- no ghost anchor is spawned.
    bool SubmitAnchorNoteForRowForTest(int sourceRow, const QString &note);

    /// Test seam replaying the post-`exec` body of `GotoLine`
    /// without a modal dialog. Lets tests drive the range check
    /// (shrink-while-modal-open simulation: pass a line larger
    /// than the current row count) and the filter-visibility hint.
    void ExecuteGotoLineForTest(const QString &input);

    /// Test seam replaying the post-`exec` body of `GotoTimestamp`
    /// without a modal dialog. @p now pins the clock so relative
    /// shortcuts (`-1h`) are deterministic. Covers the sticky-
    /// input update, error hints, and `FindFirstRowAtOrAfter`
    /// handoff -- none reachable through `ParseGotoTimestampInput`.
    void ExecuteGotoTimestampForTest(const QString &input, std::chrono::system_clock::time_point now);

    /// Test-only accessor for the sticky Goto Timestamp input so
    /// tests can pin the session-boundary clear.
    [[nodiscard]] QString LastGotoTimestampInputForTest() const;

    /// Test seam: force `LogModel::TimestampsAreMonotonic()` false
    /// so `FindFirstRowAtOrAfter` takes its non-monotonic branch
    /// without a real inversion. Irreversible.
    void ForceTimestampsNonMonotonicForTest();

    /// Test seam replaying `OpenAdvancedFilter`'s post-`exec` body
    /// without a modal dialog. Runs the full leaf-extraction +
    /// mirror + recompile pipeline so tests can pin the simple/
    /// advanced split behaviour end-to-end.
    void CommitAdvancedFilterForTest(loglib::FilterExpression expression)
    {
        ApplyAdvancedFilterResult(std::move(expression));
    }
#endif

protected:
    bool event(QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

    /// Re-tint every themed icon on a palette / style / theme /
    /// DPR change so a Light <-> Dark flip (or a monitor move
    /// between different scale factors) keeps the Lucide glyphs
    /// aligned with the new `QPalette::WindowText` and rasterised
    /// at the new device-pixel ratio. The companion hook in
    /// `OnThemeChanged` covers application-driven theme switches;
    /// this hook catches OS-level changes that bypass
    /// `ThemeControl` (Windows light/dark notification arrives as
    /// `QEvent::ThemeChange` and may not always be preceded by a
    /// palette diff). Same idiom as `FindRecordWidget::changeEvent`.
    void changeEvent(QEvent *event) override;

private slots:
    /// Discard the current session and return to an empty view.
    /// Bound to `actionNewSession` (Ctrl+N).
    void NewSession();
    /// Spawn a new top-level `MainWindow` sharing this manager.
    /// Heap-allocated with `Qt::WA_DeleteOnClose`. No-op in
    /// no-history mode.
    void NewWindow();
    /// Rebuild the `File -> Recent Sessions` submenu from the
    /// manager's live list. Connected to `aboutToShow` so we never
    /// paint stale entries.
    void RebuildRecentSessionsMenu();
    /// Reopen the recents entry @p uuid. Pre-flights the parse,
    /// then `NewSession` + `DoLoadConfiguration` to restore columns
    /// / filters / sort / source. Locators are streamed in `Append`
    /// mode (non-destructive on the now-empty model). On success
    /// `mAutoSaveUuid` is pinned to @p uuid so further edits update
    /// that recents entry instead of forking a new one.
    void OpenRecentSession(const QString &uuid);

    /// Shared tail of `RestoreLastSessionFromPath` and
    /// `OpenRecentSession`: stream `mCurrentSource->locators` or
    /// short-circuit on unsupported / empty sources.
    /// @p informIfNonFile picks between a silent skip (restore-on-
    /// launch, never pop a dialog on startup) and a
    /// `QMessageBox::information` (user-initiated click).
    void StreamFromCurrentSourceOrSkip(bool informIfNonFile);
    void OpenFiles();
    void OpenLogStream();
    /// Pop the `NetworkStreamDialog`, build the matching producer, and
    /// call `LogModel::BeginStreaming`.
    void OpenNetworkStream();
    /// "Save Configuration\u2026" -- writes the portable
    /// columns-only slice.
    void SaveConfiguration();
    /// "Save Session\u2026" -- writes columns + filters + sort + source.
    void SaveSession();
    /// Loads either shape; missing session fields default to inert
    /// values.
    void LoadConfiguration();

    /// "Export Filtered Rows\u2026" -- pops the export dialog and
    /// dispatches an async worker that writes the current filter
    /// slice in one of the four supported formats (`JSON Lines`,
    /// `CSV`, `Source snapshot`, `Markdown table`). Progress and
    /// cancel run through a modal-per-window `QProgressDialog`;
    /// user cancel unwinds via `slv::exports::ExportCancelled`.
    void ExportFilteredRows();

    /// Export all retained rows and view state to `.slvbundle`.
    /// Shares asynchronous export state with filtered-row export.
    void ExportSessionBundle();

    /// Show the `ConfigurationDiagnosticsDialog` (constructed lazily).
    /// A second call raises the existing instance.
    void ShowConfigurationDiagnostics();

    /// Refresh the status-bar mismatch summary. Wired to
    /// `LogModel::columnHealthChanged`; hides the button when zero
    /// mismatches are present.
    void UpdateDiagnosticsStatus();

    /// Refresh the status-bar parse-errors indicator. Hooked to
    /// `ParseErrorsDock::countChanged`; hides when the dock is empty.
    void UpdateParseErrorsStatus(int count, int droppedCount);

    /// Refresh the "*n* shown of *m*" status-bar label and toggle
    /// the inline Clear-filters button. Wired to source + proxy
    /// row signals; the label tracks `mModel->rowCount()` (not
    /// `IsSessionActive`) so the indicator survives the post-load
    /// `Static -> Idle` flip and stays visible while the user
    /// browses the parsed rows. Hides both widgets when the source
    /// model is empty.
    void UpdateRowsShownStatus();

    /// Recount matches for the current find query and push the
    /// result back into the find bar. Caches the row list keyed by
    /// `(needle, wildcards, regex)` so Next / Previous clicks reuse
    /// the scan and just resolve the new `i` via binary search.
    /// Skipped when the bar is hidden / proxy is unset / needle empty.
    void UpdateFindMatchCount(const QString &text, bool wildcards, bool regularExpressions);

    /// Drop the cached match-row list. Wired to model resets and
    /// proxy layout changes so a stale cache cannot survive.
    void InvalidateFindMatchCache();

    /// Push the current `LogSession::FindMatchCacheState` match into the
    /// overview rail. No-op when the find bar is not visible —
    /// ticks mirror the find indicator, they must not reappear
    /// from a stale cache after find was closed. Prefers cached
    /// per-bucket counts (unbiased even when `sortedRows` is
    /// capped); forces a full recount when they're missing or
    /// size-mismatched so the rail never paints a top-biased
    /// strip.
    void PushFindMatchesToOverviewRail();

    /// Centralised invalidate + debounced re-request. Wired to every
    /// model / proxy signal that can change the match set; a sync
    /// re-scan per signal would melt under streaming.
    void OnFindCacheInvalidated();

    void Find();
    void FindRecords(const QString &text, bool next, bool wildcards, bool regularExpressions);

    /// Add a filter rule, optionally opening the editor. Pass
    /// `openEditor = false` on the config-load path so a restored
    /// filter does not pop the editor. When @p filter is empty and
    /// @p initialColumn >= 0, the editor preselects that column
    /// (used by the header "Add filter on ..." entry). Ignored
    /// when @p filter has a value (it already pins the row).
    void AddFilter(
        const QString &filterId,
        const std::optional<loglib::LeafRule> &filter = std::nullopt,
        bool openEditor = true,
        int initialColumn = -1
    );
    void ClearAllFilters();
    /// Open the modal Advanced Filter editor seeded with the
    /// current `LogConfiguration::expression`. On accept, dispatches
    /// to `ApplyAdvancedFilterResult`.
    void OpenAdvancedFilter();

    /// Post-dialog body of `OpenAdvancedFilter`. Extracts top-level
    /// Leaves back into `mSimpleLeaves` (mirroring load) so a mixed
    /// tree like `svc:x AND NOT lvl:info` still shows a Filters-menu
    /// entry per representable leaf; the non-Leaf remainder stays
    /// on the expression. Exposed via `CommitAdvancedFilterForTest`.
    void ApplyAdvancedFilterResult(loglib::FilterExpression result);
    /// Drop the active column sort via
    /// `mTableView->sortByColumn(-1, ...)` so proxy, header, and
    /// persisted config stay in lockstep. Bound to
    /// `actionClearSort` (Sort menu, toolbar, status bar,
    /// header right-click).
    void ClearSort();
    /// Remove a single filter rule. Pass `deferSync = true` when the
    /// caller (e.g. a submit slot) immediately re-adds the filter
    /// so the mirror + rule rebuild only run once.
    void ClearFilter(const QString &filterID, bool deferSync = false);
    void FilterSubmitted(const QString &filterID, int row, const QString &filterString, int matchType);
    /// Slot for `FilterEditor::FilterTimeStampSubmitted`. `std::nullopt`
    /// on a bound leaves that side unbounded (the predicate substitutes
    /// INT64 sentinels at construction); both-nullopt is rejected.
    void FilterTimeStampSubmitted(
        const QString &filterID, int row, std::optional<qint64> beginTimeStamp, std::optional<qint64> endTimeStamp
    );
    void FilterEnumSubmitted(const QString &filterID, int row, const QStringList &selectedValues);
    /// Slot for `FilterEditor::FilterNumericRangeSubmitted`. Either bound
    /// may be `std::nullopt` to leave that side unbounded.
    void FilterNumericRangeSubmitted(
        const QString &filterID, int row, std::optional<double> minValue, std::optional<double> maxValue
    );
    /// Slot for `FilterEditor::FilterBooleanSubmitted`.
    void FilterBooleanSubmitted(const QString &filterID, int row, bool includeTrue, bool includeFalse);

    /// Pause / resume on the bridging sink. Bound to `actionPauseStream`.
    void TogglePauseStream(bool paused);

    /// Stop the active stream. Bound to `actionStopStream`.
    void StopStream();

    /// Rotation event re-emitted on the GUI thread; flashes the
    /// `— rotated` status-bar suffix.
    void OnRotationDetected();

    /// Producer status transition; latches `mSourceWaiting` and
    /// refreshes the status bar.
    void OnSourceStatusChanged(loglib::SourceStatus status);

    /// Translate a header drag into a source-side `LogModel::
    /// MoveColumn`, then restore visual == logical. The runtime
    /// filter remap and visibility re-apply happen in
    /// `OnSourceColumnsMoved`, which also catches implicit moves
    /// (e.g. mid-stream timestamp bubbling).
    void OnHeaderSectionMoved(int logicalIndex, int oldVisualIndex, int newVisualIndex);

    /// `LogModel::columnsMoved` slot: re-apply `Column::visible`
    /// and rebuild the compiled filter (which caches resolved
    /// column indices; leaves themselves bind by `columnKeys`).
    /// Handles both header-drag and streaming-induced column moves
    /// (the timestamp bubble in `LogModel::AppendBatch`). Qt
    /// clears hidden flags via `initializeSections()` on a
    /// zero-row source, hence the visibility re-apply.
    void OnSourceColumnsMoved(
        const QModelIndex &parent, int first, int last, const QModelIndex &destParent, int destColumn
    );

    /// Build and show the header context menu at @p pos.
    void ShowHeaderContextMenu(const QPoint &pos);

    /// Build and show the row right-click menu at @p pos (viewport
    /// coords). Adds an inclusive time-range filter pinned to the first
    /// `Type::Time` column, boundary = clicked row's timestamp.
    void ShowRowContextMenu(const QPoint &pos);

    /// Rebuild the `View` menu on each `aboutToShow`. Each column
    /// gets a checkable action that toggles `Column::visible`.
    /// Always reachable, so it can restore visibility when every
    /// header section is hidden.
    void RebuildViewMenu();

private:
    /// Single source of truth for the shell's model-quintet and
    /// view aliases (task 4.2 review-5). The ctor calls this twice:
    /// once right after `mSession = new LogSession(...)` with
    /// `view == nullptr` (view not yet constructed), and again
    /// after `mSessionView = new LogSessionView(...)` with both
    /// pointers set. Consolidating the eight-plus alias
    /// assignments here eliminates the duplication that the
    /// previous review flagged as drift bait: any future field
    /// added to the alias set is added in exactly one place.
    ///
    /// **Not** a bind / rebind entry -- this method deliberately
    /// does not touch `mSessionConnections`. The ctor owns
    /// subscription installation until phase 6 factors it into a
    /// public `RebindActiveSession(session, view)` driver that
    /// pairs a bag rebuild with an alias refresh.
    ///
    /// @param session non-null; the new active session (its
    ///                model-quintet becomes the shell's aliases).
    /// @param view    optional; when non-null, the new active
    ///                view (its `TableView` / `OverviewRail` /
    ///                `OverviewRailModelPtr` become the shell's
    ///                aliases). Pass `nullptr` during the ctor's
    ///                pre-view phase.
    void SetActiveSessionAliases(LogSession *session, LogSessionView *view) noexcept;

    /// RAII helper for the session-switch latch on `mSession`. Every
    /// destructive open path needs to flip the flag on, run a
    /// `mModel->Reset()` that synchronously emits
    /// `streamingFinished(Cancelled)`, then flip it back off once
    /// the new session is wired up. The RAII helper enforces the
    /// contract at the type level so no early-return path can
    /// forget the reset. The flag itself lives on `LogSession`
    /// (task 2.5).
    struct SessionSwitchScope
    {
        explicit SessionSwitchScope(MainWindow &owner) noexcept
            : mOwner(owner)
        {
            mOwner.mSession->SetSessionSwitchInProgress(true);
        }
        ~SessionSwitchScope()
        {
            mOwner.mSession->SetSessionSwitchInProgress(false);
        }
        SessionSwitchScope(const SessionSwitchScope &) = delete;
        SessionSwitchScope &operator=(const SessionSwitchScope &) = delete;
        SessionSwitchScope(SessionSwitchScope &&) = delete;
        SessionSwitchScope &operator=(SessionSwitchScope &&) = delete;

    private:
        MainWindow &mOwner;
    };

    /// Append the "Anchor" sub-menu (eight colour swatches +
    /// "Remove anchor") to @p menu. Check state reflects the right-
    /// clicked row's existing colour, but triggered actions operate
    /// on the current selection (same as the `Ctrl+1..8` hotkeys).
    /// No-op if model, theme, or anchor manager is missing.
    void AppendAnchorActionsToRowMenu(QMenu *menu, int sourceRow);

    /// Shared post-`exec` body of `GotoLine`. Re-checks the live
    /// row count (catches a shrink while the modal was open),
    /// probes filter visibility, then hands off to
    /// `SelectSourceRow`. Emits a status hint on every rejection.
    void ExecuteGotoLine(const QString &input);

    /// Shared post-`exec` body of `GotoTimestamp`. Updates the
    /// sticky-input mirror, parses via `ParseGotoTimestampInput`,
    /// shifts naive results through
    /// `LocalMicrosecondsSinceEpochToUtc`, then hands off to
    /// `FindFirstRowAtOrAfter` + `SelectSourceRow`. @p now is
    /// pinned in tests, `system_clock::now()` in production.
    void ExecuteGotoTimestamp(const QString &input, std::chrono::system_clock::time_point now);

    /// Logical index of the column whose `keys` match @p keys, or
    /// `-1` if none. `keys` is the only identifier that survives a
    /// reorder; menu lambdas use it to re-resolve the target column
    /// at trigger time.
    [[nodiscard]] int FindColumnIndexByKeys(const std::vector<std::string> &keys) const;

    /// Menu label for one column: the header, or `header [keys]`
    /// when the header is shared with another column. Empty when
    /// @p columnIndex is out of range. For all columns at once,
    /// prefer `BuildAllColumnMenuLabels` (this entry point is O(N)
    /// per call).
    [[nodiscard]] QString ColumnMenuLabel(size_t columnIndex) const;

    /// Menu labels for every column in one O(N) pass (tallies
    /// duplicate headers once and reuses the count). Use this from
    /// the `View` menu rebuild instead of looping `ColumnMenuLabel`.
    [[nodiscard]] std::vector<QString> BuildAllColumnMenuLabels() const;

    /// Try to load @p file as a `LogConfiguration`; returns true on
    /// success.
    bool TryLoadAsConfiguration(const QString &file);

    /// Funnel for drop / Open... / CLI inputs that may mix
    /// configuration JSONs and log files. Each path is classified
    /// via `FileLooksLikeConfiguration`:
    ///
    /// - Zero configs -> `StartStreamingOpenQueue(files, logMode)`.
    /// - One config, no logs -> `TryLoadAsConfiguration` (no reset).
    /// - One config + N logs -> `DoLoadConfiguration` (full reset)
    ///   then `StartStreamingOpenQueue(logs, Append)`.
    /// - Two or more configs -> warning dialog, no state mutated.
    ///
    /// @p logMode is used only for the no-config branch (the mixed
    /// branch is always `Append` since `DoLoadConfiguration` already
    /// did the reset).
    ///
    /// The returned `appliedConfigPath` names the configuration the
    /// dispatcher actually picked (not necessarily `files.front()`),
    /// so callers can name it correctly in user-facing text.
    MixedInputResult DispatchMixedOpenInput(const QStringList &files, OpenMode logMode);

    /// Start a sequential streaming open of @p files.
    ///
    /// `OpenMode::Replace`: reset the model, clear runtime filters,
    /// drop `mCurrentSource`, then queue the files (first via
    /// `BeginStreaming`, subsequent via `AppendStreaming`).
    ///
    /// `OpenMode::Append`: keep the active static session intact and
    /// queue @p files onto the back. With no active session it
    /// behaves like `Replace` minus the destructive reset (so
    /// previously-loaded columns / filters survive into the new
    /// session). Live-tail / network sessions always force `Replace`.
    void StartStreamingOpenQueue(QStringList files, OpenMode mode);

    /// Pop the next file off `mSession->MutablePendingOpenFiles()`
    /// and parse it. Open errors accumulate in
    /// `mSession->MutablePendingOpenErrors()`; decompression
    /// failures land in
    /// `mSession->MutablePendingDecompressionErrors()` (drained under
    /// its own title).
    ///
    /// For compressed files the function spawns an async
    /// `DecompressingByteSource` worker and returns before the
    /// parse begins; `OnDecompressionFinished` re-enters the
    /// remainder of the open path.
    void StreamNextPendingFile();

    /// Dispatch an async `DecompressingByteSource` worker for
    /// @p originalPath after the sniff has decided it is
    /// compressed. Wires up the progress dialog, poll timer, and
    /// watcher; the worker runs on the Qt thread pool. Called
    /// from `StreamNextPendingFile`; the finished slot
    /// (`OnDecompressionFinished`) picks the flow back up on the
    /// GUI thread.
    void BeginAsyncDecompression(const QString &originalPath, loglib::internal::DecompressingByteSource::Codec codec);

    /// Continuation of `StreamNextPendingFile` for compressed
    /// inputs: takes ownership of the worker-produced
    /// `DecompressingByteSource`, resumes the parse (or the queue
    /// drain, on failure) on the GUI thread.
    void OnDecompressionFinished();

    /// GUI-thread continuation of a successful open. Constructs
    /// `LogFile`, detects format, and hands off to
    /// `LogModel::BeginStreaming` / `AppendStreaming`. Shared by
    /// the uncompressed fast path and the decompression continuation.
    ///
    /// @p originalPath is the user-facing path (window title, status
    /// bar, session locators). @p effectivePath is what downstream
    /// code mmaps and probes (equal to @p originalPath when
    /// uncompressed). @p decompressionAnchor keeps a decompressed
    /// temp file alive for the parse's duration; nullptr for
    /// uncompressed opens.
    ///
    /// Returns `true` when a parse worker was armed (caller unwinds
    /// and awaits `streamingFinished`) or `false` on a synchronous
    /// open error already recorded in the appropriate error bucket
    /// (caller continues draining
    /// `mSession->MutablePendingOpenFiles()`). This return
    /// value keeps the queue drain iterative instead of recursing
    /// on error.
    [[nodiscard]] bool ContinueOpenAfterPrepared(
        const QString &originalPath,
        const std::filesystem::path &effectivePath,
        std::shared_ptr<loglib::internal::DecompressingByteSource> decompressionAnchor
    );

    /// Show the modal progress dialog + spin up the poll timer for
    /// the current decompression. Called from `StreamNextPendingFile`
    /// after the sniff decides the file is compressed and before
    /// the worker is dispatched.
    void ShowDecompressionProgress();

    /// Tear down the progress dialog + poll timer created by
    /// `ShowDecompressionProgress`. Idempotent; safe to call from
    /// the finished slot and from destructive teardown paths.
    void TeardownDecompressionProgress();

    /// Cancel + drain any in-flight decompression worker and detach
    /// its future so a queued `finished` signal cannot fire against
    /// a re-armed session. Called at every destructive session
    /// boundary; without this an orphaned worker would splice the
    /// old file into the new session. Also clears
    /// `mDecompressionOriginalPath` and the decompression error
    /// bucket. Idempotent. Anchor cleanup is separate: each anchor
    /// is attached to its `LogFile` and released when the model
    /// drops that FileLineSource.
    void CancelInFlightDecompression();

    /// Runs the `OnStreamingFinished` teardown when the queue drains
    /// through a decompression that did NOT hand off to a parse
    /// worker (e.g. the last queued file failed decompression).
    /// Without this, error buckets accumulate silently and
    /// `mSessionMode` stays `Static` with no live worker, leaving
    /// the config UI greyed out until the user forces a reset.
    ///
    /// No-op while another worker is still in flight -- the natural
    /// drain point (`OnStreamingFinished` or the next
    /// `OnDecompressionFinished`) will run instead. Rows +
    /// `mCurrentSource` are preserved; auto-save runs if the
    /// surviving session shape is restorable.
    void FinalizeAfterDecompressionIfChainTerminal();

    /// Slot for `LogModel::streamingFinished`. Hoisted out of an
    /// inline lambda so crash-dump frames identify it by name and
    /// tests can exercise the post-streaming reset logic directly.
    /// Owns queue draining, session-mode reset, auto-save publish,
    /// and parse-error surfacing.
    void OnStreamingFinished(StreamingResult result);

    /// Route @p errors under @p title to the parse-errors dock,
    /// attributing them to @p originatingSession (nullptr => the
    /// currently-active session, matching the phase-5 shell).
    /// Phase 6 tab-switch callers that complete a background parse
    /// MUST pass the originating session so the batch lands in
    /// that session's log rather than the currently-visible tab.
    void ShowParseErrors(
        const QString &title, const std::vector<std::string> &errors, LogSession *originatingSession = nullptr
    );

    /// Pop a warning dialog summarising filters dropped on load.
    /// Records @p droppedCount for tests and skips the modal when
    /// `mSuppressDialogsForTest` is set.
    void ShowDroppedFiltersDialog(int droppedCount, const QString &message);

    /// Add @p filter to `mSimpleLeaves` and build its menu entry.
    /// Bulk callers pass `deferSync = true` and run one trailing
    /// mirror + `UpdateFilters` after the loop.
    void AddLogFilter(const QString &id, const loglib::LeafRule &filter, bool deferSync = false);

    /// Display title for @p filter (e.g. `info, warn` for enum,
    /// `[1.5, 2.0]` for a numeric range). Shared by the Filters
    /// menu and the column-header right-click menu.
    [[nodiscard]] QString BuildFilterTitle(const loglib::LeafRule &filter) const;

    /// Recompile `LogConfiguration::expression` against the
    /// current column layout and install it on the proxy. Called
    /// after every mutation and on column/enum-column changes.
    void UpdateFilters();

    /// True iff the window is worth auto-saving: history manager
    /// attached, `File`-kind source with at least one locator, and
    /// a static (re-openable) session. Live-tail / stream sessions
    /// can't be restored from a JSON snapshot (the producer is
    /// stateful), so we skip them. Takes the just-finished mode
    /// explicitly because `streamingFinished` resets `mSessionMode`
    /// to `Idle` before the auto-save hook runs.
    [[nodiscard]] bool ShouldAutoSaveSession(SessionMode justFinishedMode) const;

    /// Drop `mAutoSaveUuid` from the persisted open-windows set and
    /// clear the field. Called from every state-discarding path so
    /// the next AutoSave produces a fresh entry instead of
    /// overwriting the previous session's JSON.
    void DetachAutoSaveUuid();

    /// Snapshot `mSimpleLeaves` (ordered by `mSimpleLeafOrder`),
    /// proxy sort, and `mCurrentSource` into the configuration.
    /// Simple-mode leaves become the top-level `And` children;
    /// pre-existing non-Leaf siblings (Advanced-editor output) are
    /// preserved. Bulk callers set `deferSync = true` and mirror
    /// once at the end.
    void MirrorSessionStateToConfiguration();

    /// Shared tail of `OpenLogStream` and `OpenLogStreamForTest`:
    /// runs the actual open (producer construction, flush-and-reset
    /// of the previous session, BeginStreaming) on @p file. Pulled
    /// out so tests can drive the post-dialog path without a modal
    /// `QFileDialog`.
    void OpenLogStreamFromPath(const QString &file);

    /// Attach the pending live tail after its static prefix drains.
    void ContinueLiveTailAfterPrefix();

    /// Global rotation-history preference after applying the CLI
    /// override. Session-level gating is handled separately.
    [[nodiscard]] bool ShouldAutoDetectRotationHistory() const;

    /// Rotation-history preference after global, CLI, and session gates.
    [[nodiscard]] bool EffectiveAutoDetectRotationHistory() const;

    /// Clear expansion-undo state at a destructive session boundary.
    void ClearRotationExpansionUndoState() noexcept;

    /// Clear a pending historical-prefix promotion so it cannot
    /// attach to a later session.
    void ClearPendingLiveTailPromotion() noexcept;

    /// Controls whether expansion consults the current source's
    /// opt-out and locator deduplication state.
    enum class RotationSourceGating
    {
        /// Apply both the session opt-out and loaded-locator deduplication.
        HonourAll,
        /// Ignore the outgoing source during a destructive replacement.
        Ignore,
        /// Apply the session opt-out but ignore stale locator keys.
        HonourOptOutOnly,
    };

    /// Expand rotation families in oldest-first order and deduplicate
    /// them according to @p gating. Bundles pass through unchanged.
    /// @p addedOut counts only auto-discovered paths absent from the
    /// input. @p primaryOut receives the first expanded family's
    /// canonical primary, or remains empty when nothing was added.
    [[nodiscard]] QStringList ExpandLogPathsWithRotationSiblings(
        const QStringList &logPaths, int &addedOut, RotationSourceGating gating, QString *primaryOut = nullptr
    ) const;

    /// Report an expansion and enable its Undo action.
    void ShowRotationHistoryToast(int addedCount, const QString &primary);

    /// Reopen the original inputs without rotation expansion.
    void UndoRotationExpansion();

    /// Sync the Settings action to the effective preference.
    void SyncRotationHistoryActionCheckedState();

    /// Persist the preference and mirror it to the current source.
    void OnRotationHistoryPrefToggled(bool enabled);

    /// Path-based save / load shared by the dialog slots and the
    /// test seams. `DoSaveConfiguration` mirrors session state and
    /// writes the slice selected by @p scope; throws on failure.
    /// `DoLoadConfiguration` parses the file, then resets the model,
    /// validates saved filters, restores sort. Returns false on
    /// parse error. Detaches `mAutoSaveUuid` so the next AutoSave
    /// creates a fresh recents entry instead of overwriting an
    /// unrelated prior session; callers that want to re-pin
    /// (`OpenRecentSession`, `RestoreLastSessionFromPath`) must do
    /// so explicitly after a successful load.
    void DoSaveConfiguration(const QString &path, loglib::SaveScope scope);
    bool DoLoadConfiguration(const QString &path);

    /// Apply an already-parsed `LogConfiguration` to the live model.
    /// Shared tail of `DoLoadConfiguration`. Destructive: clears
    /// proxy rules + sort, resets the model, replaces the
    /// configuration, and rebuilds filters. Returns false (with a
    /// warning dialog) if the apply step throws.
    bool ApplyLoadedConfiguration(loglib::LogConfiguration parsed);

    /// Re-validate every saved filter against the freshly-loaded
    /// columns and revive survivors via `AddLogFilter`. Shared by
    /// `DoLoadConfiguration` and `TryLoadAsConfiguration`.
    void RebuildFiltersFromConfiguration();

    /// Drop simple-mode leaves, per-filter menu entries, and the
    /// "Clear All Filters" gate. Does *not* touch
    /// `LogConfiguration::expression`, mark dirty, or refresh the
    /// mirror/indicators -- callers handle those (bulk load runs one
    /// mirror at the end; `ClearAllFilters` runs its own refresh).
    /// Split out so `ClearAllFilters` can additionally reset the
    /// expression tree while the load path preserves it.
    void ResetSimpleFilterState();

    /// Gate "Clear All Filters" on
    /// `LogConfiguration::expression` (not `mSimpleLeaves`) so an
    /// Advanced-only tree still enables the escape hatch. Must run
    /// after `MirrorSessionStateToConfiguration`.
    void SyncClearAllFiltersEnabled();
    void ApplyTableStyleSheet();

    /// Pick the light- or dark-variant title-bar icon to match the
    /// active theme.
    void ApplyThemedWindowIcon();

    /// Slot for `ThemeControl::themeChanged()`. Re-applies the
    /// table QSS and repaints the viewport so cells re-query
    /// `data()` for the new per-level brushes / fonts.
    void OnThemeChanged();

    /// Canonical `EnumDictionary` for @p columnIndex; nullptr when the
    /// column is not promoted or has no keys.
    [[nodiscard]] const loglib::EnumDictionary *ResolveEnumDictionary(int columnIndex) const;

    /// True iff every selected string in @p filter resolves to an id
    /// in the canonical dictionary. Gates whether an
    /// `enumColumnsChanged` tick triggers a filter-rule rebuild.
    [[nodiscard]] bool EnumFilterFullyResolved(const loglib::LeafRule &filter) const;

    /// Apply the saved sort from
    /// `LogSession::HasPendingApplySortFromConfig()` to the view,
    /// then clear the latch. No-op when the latch is clear, when
    /// the user sorted mid-stream
    /// (`SortColumn() >= 0`), or when the saved column is
    /// out-of-range.
    ///
    /// Called from `OnStreamingFinished` and from
    /// `StreamFromCurrentSourceOrSkip`'s early-return paths. The
    /// deferral lets streaming use the fast bulk-insert branch and
    /// then sorts once over the full row set instead of paying
    /// O(N^2) per-row inserts under an active sort.
    void ApplyDeferredSortFromConfig();

    void SetConfigurationUiEnabled(bool enabled);
    void UpdateStreamingStatus();

    /// Starts the elapsed-time timer and 1 Hz refresh tick for live tail.
    void StartLiveTailTicker();

    /// Stops the 1 Hz tick but keeps the elapsed value for the final status.
    void StopLiveTailTicker();

    /// Opens (or raises) the modeless shortcuts dialog, building it lazily.
    void ShowShortcutsDialog();

    /// Persists window geometry and dock layout to `QSettings`.
    void SaveWindowChrome() const;

    /// Restores window geometry and dock layout from `QSettings`.
    /// Must run after every dock/toolbar widget has its `objectName`
    /// so `restoreState` can resolve them.
    void RestoreWindowChrome();

    /// Rebuilds the window title from the current session state.
    void UpdateWindowTitle();

    /// Marks filters as having unsaved edits on the active session.
    /// Thin forward to `LogSession::MarkFiltersDirty()`, which is a
    /// no-op while `LogSession::IsLoadingConfiguration()` is true so
    /// a config reload doesn't transiently flash `[*]`. The window
    /// title is refreshed via the `filtersDirtyChanged` signal.
    void MarkFiltersDirty();

    /// Last-used dialog directory, or the platform `Documents` location
    /// on first run. Persisted in `QSettings` under `ui/lastOpenDir`.
    [[nodiscard]] QString DefaultOpenDir() const;

    /// Persists the directory of @p path as the last-used dialog dir.
    void RememberLastOpenDir(const QString &path);

    /// Last-used export directory (`ui/lastExportDir`), kept
    /// separate from `ui/lastOpenDir` so a one-off export to a
    /// shared drive doesn't retarget the next `File -> Open...`
    /// dialog. Falls back to `DefaultOpenDir()` on first use.
    [[nodiscard]] QString DefaultExportDir() const;

    /// Persists the directory of @p path under `ui/lastExportDir`.
    void RememberLastExportDir(const QString &path);

    /// Appends shortcut text to each action's tooltip and mirrors the
    /// tooltip into `statusTip()`. Skips actions whose tooltip already
    /// names the shortcut, so it's safe to re-run.
    void FinaliseActionMetadata();

    /// Build the persistent primary `QToolBar` and `insertToolBar`
    /// it ahead of `mStreamToolbar` so the two bars share the top
    /// dock area as one strip (main first, stream second). Tags
    /// every populated action with a `svgIconPath` property (and,
    /// where applicable, `svgIconPathChecked` for a distinct
    /// On-state glyph) so `RefreshThemedIcons` can re-tint without
    /// a per-action switch. Called once at the end of the
    /// constructor, after `mStreamToolbar`, `mActionToggleFind`
    /// and `mActionToggleAnchors` are wired (every action the
    /// builder references must already exist) but before
    /// `RestoreWindowChrome` reads the persisted dock state.
    void BuildMainToolbar();

    /// Re-render every themed icon at the current palette
    /// `WindowText` colour and device-pixel ratio. Walks
    /// `mThemedActions` (every entry was registered with its
    /// preferred anchor widget at `BuildMainToolbar` time) and
    /// reads the `svgIconPath` / `svgIconPathChecked` properties
    /// each action carries. Actions without the property are
    /// skipped, so it's safe to run before `BuildMainToolbar`
    /// (no-op when `mMainToolbar` is still null) and idempotent
    /// under duplicate triggers (theme switch + DPR change firing
    /// within the same event loop pass).
    ///
    /// Anchor-driven (not toolbar-iteration-driven) so actions
    /// reached through `QToolBar::addWidget` (the open-stream
    /// split button's default action, its popup-menu entries)
    /// participate in the refresh -- those are wrapped in an
    /// internal `QWidgetAction` that does NOT appear in the
    /// toolbar's `actions()` list and would otherwise be missed,
    /// leaving the split button blank on theme flip.
    void RefreshThemedIcons();

    /// Repopulate the Add-filter split-button dropdown with one
    /// `Add filter on "<col>"…` entry per *visible* column.
    /// Connected to the menu's `aboutToShow` so the listing
    /// always reflects the current configuration without us
    /// having to invalidate it from every column-mutation site
    /// (`SetColumnVisible`, `OnSourceColumnsMoved`,
    /// `ColumnsManagerDialog::Accept`, post-stream column
    /// promotion, …). The clicked entry routes through the same
    /// `AddFilter(uuid, nullopt, openEditor=true, initialColumn=idx)`
    /// path the header right-click uses, so column reorders
    /// between menu build and click resolve via the stable `keys`
    /// captured in the lambda.
    ///
    /// Hidden columns are skipped (`SetInitialColumn` refuses to
    /// preselect them, mirroring the header context menu) and
    /// each entry is disabled when the model has no rows
    /// (`AddFilter` would short-circuit with a status-bar hint).
    /// An empty configuration produces a single disabled
    /// `(no columns yet)` placeholder so the dropdown is never
    /// silently empty.
    void RebuildAddFilterMenu(QMenu *menu);

    /// Repopulate `menuSort`: `actionClearSort` + separator,
    /// then two checkable rows per visible column
    /// (`▲ "<col>"` / `▼ "<col>"`) whose check state mirrors
    /// the proxy's current sort. Hooked to `aboutToShow` so the
    /// menu always reflects the live configuration.
    void RebuildSortMenu();

    /// Repopulate the Sort split-button dropdown with the same
    /// per-column rows as `RebuildSortMenu`, minus the
    /// Clear-sort row (the toolbar has its own Clear-Sort button
    /// next to this one).
    void RebuildSortByMenu(QMenu *menu);

    /// Append two checkable rows per visible column to @p menu
    /// (`▲ "<col>"` / `▼ "<col>"`) whose check state mirrors
    /// the proxy's sort. The triangle is the same glyph
    /// `QHeaderView` paints as its sort indicator. Rows are
    /// disabled when the model has no rows or the column's data
    /// doesn't match its configured type; disabled rows carry a
    /// tooltip pointing at Configuration Diagnostics. Shared
    /// core for `RebuildSortMenu` and `RebuildSortByMenu`;
    /// returns true if any row was added (so the caller can
    /// fall back to a placeholder when every column is hidden).
    bool AppendSortByEntries(QMenu *menu);

    /// `AppendSortByEntries` plus the placeholder fallbacks both
    /// Sort surfaces share: `(no columns yet)` for an empty
    /// configuration and `(every column is hidden ...)` when
    /// nothing visible remains.
    void AppendSortEntriesOrPlaceholder(QMenu *menu);

    /// Sync `actionClearSort`'s enabled state and
    /// `mClearSortStatusButton`'s visibility with the proxy's
    /// current sort. Hidden when the model is empty; visible
    /// while a sort is active. Hooked to `layoutChanged`, the
    /// source's row signals, and horizontal `headerDataChanged`
    /// (so a column rename refreshes the tooltip without
    /// waiting for the next sort / filter event).
    void UpdateSortStatus();

    /// Repopulate the Clear-filters split-button dropdown with
    /// one `Remove "<col>": <title>` entry per active filter,
    /// grouped by column index then sorted by display title.
    /// Connected to the menu's `aboutToShow`; we don't have to
    /// invalidate it from `AddLogFilter` / `ClearFilter` /
    /// `ClearAllFilters` because the menu is rebuilt every time
    /// it's opened.
    ///
    /// When `mSimpleLeaves` is empty the menu shows a single disabled
    /// `(no filters)` placeholder so the dropdown surfaces a
    /// hint instead of opening blank. (The button's default
    /// action stays gated by `actionClearAllFilters->setDisabled`
    /// in the empty-filters branch; on the styles where the
    /// menu arrow shares the disabled state with the button face
    /// the dropdown won't open at all -- that's a graceful
    /// degradation, not a regression, since there's nothing to
    /// remove.)
    void RebuildClearFiltersMenu(QMenu *menu);

    /// Snapshot active filter titles per column from `mSimpleLeaves`
    /// and push them into `LogModel::SetColumnFilterDetails`,
    /// which drives the funnel decoration + "Filters:" tooltip
    /// section. Sorts each column's titles for stable display.
    ///
    /// Called from every `mSimpleLeaves` mutation point and from
    /// column-shape signals that can shift a column's resolved
    /// index. Idempotent via the model-side diff guard.
    void SyncColumnFilterIndicators();

    /// Re-evaluate the stream toolbar's visibility against the current
    /// session mode.
    void UpdateStreamToolbarVisibility();

    /// Scroll to the newest row when Follow newest is on. Thin
    /// gate that forwards to `JumpToNewestRow`, which is what
    /// actually handles the proxy chain and the filtered fallback.
    void ScrollToNewestRowIfFollowing();

    /// Scroll to the newest row through the proxy chain, ignoring
    /// session mode / `actionFollowTail`. Used by the pill click
    /// ("catch me up") rather than the streaming-policy state
    /// machine. Safe to call with no rows.
    ///
    /// Target resolution:
    ///   1. Map the source-newest row through the proxy chain.
    ///      Correct under custom column sorts.
    ///   2. If filtered out, walk source rows backwards up to
    ///      `JUMP_FALLBACK_WALK_LIMIT` and take the first that
    ///      survives the proxy.
    ///   3. If nothing visible, snap to the proxy's visual tail
    ///      (`LogTableView::GetTailEdge()`) so the click always
    ///      moves the viewport.
    void JumpToNewestRow();

    /// Re-apply the persisted retention cap to the model.
    void ApplyStreamingRetention();

    /// Connect the current selection model to the Record Details refresh
    /// slot. Must be re-called after any `setModel` on the table view --
    /// Qt destroys the old selection model and severs prior connections.
    /// Uses `Qt::UniqueConnection`, so repeat calls are idempotent.
    void RebindRecordDetailSelectionTracking();

    /// True iff the find dock is realised and visible. Tabified-dock
    /// semantics: the inactive tab of a tabified group reports
    /// `isVisible() == false`, which is exactly what we want -- no
    /// match-count recount when the indicator can't be seen. The null
    /// check guards constructor-phase and shutdown races.
    [[nodiscard]] bool IsFindBarVisible() const noexcept
    {
        return mFindDock != nullptr && mFindDock->isVisible();
    }

    /// True iff the find dock is visible AND holds keyboard focus.
    /// Used by the parse-errors auto-raise guard and `Find()`'s smart
    /// toggle (Ctrl+F closes only when focus is already in the bar).
    [[nodiscard]] bool FindBarHoldsFocus() const noexcept
    {
        return IsFindBarVisible() && mFindDock->isAncestorOf(QApplication::focusWidget());
    }

    /// Wire the standard dock toggle pattern: `action->toggled` opens
    /// (`onShow`, default show+raise) or closes (`close()`) the dock;
    /// `visibilityChanged(true)` re-checks the action; `closedSignal`
    /// un-checks it on genuine dismissal. Splitting on/off across
    /// these two signals is what lets the menu checkmark survive
    /// tab switches in a tabified group.
    ///
    /// `onShown` runs after the action is re-checked and is the hook
    /// for per-dock catch-up work (selection refresh, match count, ...).
    template <typename DockT>
    void WireDockToggle(
        DockT *dock,
        QAction *action,
        void (DockT::*closedSignal)(),
        const std::function<void()> &onShow = {},
        const std::function<void()> &onShown = {}
    );

    Ui::MainWindow *ui;
    QVBoxLayout *mLayout;

    /// Non-visual owner of the model quintet (task 2.1) and, in
    /// later Phase 2 subtasks, of the source lifecycle / filters /
    /// workers / persistence identity. `MainWindow` constructs
    /// exactly one `LogSession` during its own ctor and keeps the
    /// existing `mModel` / `mAnchors` / etc. members as
    /// *temporary* delegated aliases so incremental extraction stays
    /// buildable. Later phases collapse the aliases and route
    /// commands through `mSession->` directly.
    LogSession *mSession = nullptr;

    /// Per-tab visual workspace (task 3.1 / 3.9). Owns the
    /// `LogTableView`, `OverviewRailModel`, and
    /// `OverviewRailWidget`; the shell's `mTableView` /
    /// `mOverviewRailModel` / `mOverviewRailWidget` pointers are
    /// non-owning aliases into this object. Parented on the
    /// central widget so it dies with the window shell; Phase 6
    /// promotes it into one entry of a `QTabWidget`-backed
    /// workspace and this direct member becomes the active-tab
    /// alias.
    /// `QPointer` (post-review-3): the ~20 `mSessionView != nullptr`
    /// guards scattered through the shell body previously could
    /// not fire because a raw pointer to a destroyed child stays
    /// non-null. `QPointer` clears the slot on Qt-destroyed
    /// callback, so the guards now mean "session view still alive"
    /// -- which matters during shell teardown races (dialog
    /// callbacks, queued signals) and after phase 6's per-tab
    /// destroy path lands.
    QPointer<LogSessionView> mSessionView;

    /// Scoped bag of every shell-side subscription that references
    /// the currently-active session, model quintet, or view (task
    /// 4.2). Populated inline by the ctor via
    /// `mSessionConnections += connect(...)`; cleared by the
    /// destructor's own `mSessionConnections.Clear()` (before
    /// `mSession` and `mSessionView` go away) and by
    /// `UnbindActiveSessionForTest()` for the Unbind coverage pin
    /// in `session_tabs_test.cpp`.
    ///
    /// **Phase 4 does not expose a public bind entry** -- the
    /// previous review flagged an aspirational `BindActiveSession`
    /// that cleared the bag without reinstalling as a foot-gun,
    /// and removing it entirely is safer than shipping a public
    /// method that bricks the window. Phase 6 will factor the
    /// ctor's subscribe block into a shared helper called from a
    /// new `RebindActiveSession(session, view)` driver that pairs
    /// alias refresh (via `SetActiveSessionAliases`) with a full
    /// bag rebuild against the newly-active pair.
    ///
    /// Membership rule: the bag holds every connection that
    /// **either**
    ///
    ///   1. has a session-owned or view-owned QObject as
    ///      sender/receiver, **or**
    ///   2. has both endpoints shell-scoped but whose slot body
    ///      dereferences a session/view alias (`mModel`,
    ///      `mTableView`, `mOverviewRailWidget`, ...). The alias
    ///      value must be re-resolved on a phase-6 tab switch,
    ///      and reinstalling the connect is how that happens.
    ///
    /// `ui->actionFollowTail::toggled` (shell action, `this`
    /// receiver, lambda body reads `mTableView`) is the canonical
    /// example of the second category; the phase-4 review-5 pass
    /// added it to the bag deliberately after the earlier
    /// review-4 pass flagged it as looking like an accidental
    /// bag entry.
    ///
    /// The bag deliberately does NOT hold connections whose
    /// endpoints are both shell-scoped AND whose slot bodies
    /// touch only shell-scoped state (dock toggles, status-bar
    /// buttons, most `ui->action*` triggers routed to shell
    /// slots). Those stay live for the window's lifetime and Qt
    /// reaps them via the standard child-destruction walk.
    ScopedConnections mSessionConnections;

    /// Non-owning aliases into the session-owned model quintet
    /// (task 2.1; review finding #11). Each of these points at a
    /// `QObject` that `mSession` constructs and reaps in reverse
    /// order via `~LogSession()`; the aliases are cached in the
    /// window ctor so the shell body reads `mModel->` instead of
    /// `mSession->Model()->` on hot paths.
    ///
    /// Lifetime for the window body: the aliases are valid from
    /// the end of the ctor until the ``~MainWindow()`` body
    /// finishes. The destructor uses them (``mModel->Reset()``
    /// under a ``SessionSwitchScope``) before ``mSession`` itself
    /// is destroyed.
    ///
    /// Lifetime past ``~MainWindow()`` body: Qt6's
    /// ``QObjectPrivate::deleteChildren`` walks direct children in
    /// forward registration order. The ctor body creates them in
    /// this order:
    ///
    ///     1. ``centralWidget`` is created by ``ui->setupUi(this)``
    ///        (first line of the ctor body), and later reparented
    ///        to host ``mSessionView`` + the view's children
    ///        (``mTableView``, ``mOverviewRailWidget``,
    ///        ``mOverviewRailModel``).
    ///     2. ``mSession`` is created immediately after
    ///        ``setupUi`` returns (``new LogSession(..., this)``),
    ///        so it registers as the *second* direct child of
    ///        ``MainWindow``.
    ///     3. Docks (``mAnchorsDock``, ``mHistogramDock``,
    ///        ``mFindDock``, ``mParseErrorsDock``,
    ///        ``mRecordDetailDock``, ...) are constructed further
    ///        down the ctor body, so they register *after*
    ///        ``mSession``.
    ///
    /// Forward-order reap is therefore:
    ///
    ///     1. ``~centralWidget`` -> ``~mSessionView`` -> view
    ///        child sweep destroys ``mTableView``,
    ///        ``mOverviewRailWidget``, ``mOverviewRailModel`` (and
    ///        every widget the view owns).
    ///     2. ``~mSession`` -> the model quintet
    ///        (``AnchorManager``, ``HighlightRuleSet``,
    ///        ``LogModel``, ``RowOrderProxyModel``,
    ///        ``LogFilterModel``) is destroyed here, in the
    ///        reverse order ``LogSession``'s ctor constructed
    ///        them.
    ///     3. Docks / dialogs registered under ``this`` are
    ///        reaped in their own registration order
    ///        (``mAnchorsDock``, ``mHistogramDock``,
    ///        ``mParseErrorsDock``, ``mFindDock``, ...) --
    ///        **after** ``mSession`` has already destroyed the
    ///        quintet.
    ///
    /// Consumer QObjects that hold non-owning raw pointers into
    /// the quintet split into two categories:
    ///
    ///   * **Reaped before ``mSession`` (safe by construction)**:
    ///     the view subtree (``mTableView``, ``mOverviewRailModel``,
    ///     ``mOverviewRailWidget``), because ``centralWidget`` sits
    ///     ahead of ``mSession`` in the registration order.
    ///     ``mLevelCellDelegate`` is parented on ``mTableView`` so
    ///     it also falls in this bucket.
    ///   * **Reaped after ``mSession`` (safe only via Qt's
    ///     disconnect machinery)**: every dock plus every dialog
    ///     parented directly on ``this``. Their compiler-generated
    ///     destructors must not dereference ``mModel`` /
    ///     ``mAnchors`` / ``mHighlights`` / ``mRowOrderProxyModel``
    ///     / ``mSortFilterProxyModel``, because those objects have
    ///     already been destroyed by ``~mSession``. Today this
    ///     works because (a) the docks' destructors are
    ///     compiler-generated no-ops for the raw-pointer members,
    ///     and (b) ``QObject::destroyed`` auto-disconnects any
    ///     lingering signal wires when the quintet dies during
    ///     step 2. A future contributor adding an explicit
    ///     ``disconnect(mModel, ...)`` or any deref to a dock
    ///     destructor must either move that dock ahead of
    ///     ``mSession`` in the registration order or hold the
    ///     pointer as ``QPointer<LogModel>`` so the deref becomes
    ///     a null-check post-``~mSession``. See
    ///     ``LogSession::RowOrderProxy`` / ``FilterProxy`` /
    ///     ``Model``.
    ///
    /// The destructor's explicit teardown ordering (drain workers
    /// under a ``SessionSwitchScope``, reset via ``mModel->Reset()``,
    /// clear ``mSessionConnections``) all runs inside the
    /// ``~MainWindow()`` *body* before ``QObject``'s child sweep
    /// starts, so the aliases are still valid there. It is the
    /// dock-destructor phase that has to defer to Qt's disconnect
    /// machinery.
    RowOrderProxyModel *mRowOrderProxyModel = nullptr;
    LogFilterModel *mSortFilterProxyModel = nullptr;
    /// Non-owning alias into `mSessionView->TableView()` (task
    /// 3.2). The table widget is a child of `mSessionView`, not
    /// the shell; teardown flows `~MainWindow` → central widget
    /// destroy → `~LogSessionView` → child sweep reaches the
    /// table. Non-null after ctor.
    LogTableView *mTableView = nullptr;
    LogModel *mModel = nullptr;
    /// Icon-pill delegate for the level column. Owned via Qt
    /// parentage; `nullptr` in the no-theme test fixture path
    /// (icon mode is skipped there).
    class LevelCellDelegate *mLevelCellDelegate = nullptr;

    // Note: `mInstalledLevelDelegateColumn` moved to
    // `LogSessionView` (task 3.5). The view enforces the
    // detach-before-reinstall-on-a-new-column invariant now; the
    // shell just forwards through `ApplyLevelCellDelegate`.
    /// Dockable find bar (owned via `QMainWindow` parentage).
    /// `mFindRecord` is the hosted widget. `QPointer` on both so
    /// model / proxy signals that fire during shutdown find them
    /// null instead of dangling.
    QPointer<FindDock> mFindDock;
    QPointer<FindRecordWidget> mFindRecord;
    /// Dockable replacement for the old `QMessageBox::warning`
    /// parse-error popups. Hidden by default; auto-raised on the
    /// first error of a session.
    QPointer<ParseErrorsDock> mParseErrorsDock;
    /// Toggle action for `mFindDock`, mirrored onto the View menu.
    /// Programmatic because the .ui has no entry.
    QAction *mActionToggleFind = nullptr;
    /// Toggle action for `mParseErrorsDock`.
    QAction *mActionToggleParseErrors = nullptr;

    /// Checkable rotation-history Settings action.
    QAction *mActionAutoDetectRotationHistory = nullptr;

    /// Enabled while the current session can undo its expansion.
    QAction *mActionUndoRotationExpansion = nullptr;
    /// Status-bar indicator that surfaces when the parse-errors dock
    /// has entries; clicking it opens the dock.
    QPushButton *mParseErrorsStatusButton = nullptr;

    /// Cap on `sortedRows` (the vector powering the "*i* of *N*"
    /// binary search). Past this many hits the scan may early-exit
    /// once the rail's presence fold is settled, keeping the GUI
    /// bounded on huge tables with a common needle. When
    /// `overflowed` is set, `totalMatches` is a lower bound and
    /// the position lookup degrades for match `#10 001` or later.
    // Note: the `MAX_FIND_MATCH_COUNT` cap now lives on
    // `LogSession::MAX_FIND_MATCH_COUNT`; the alias below keeps the
    // legacy unqualified spelling available in the shell body.
    static constexpr int MAX_FIND_MATCH_COUNT = LogSession::MAX_FIND_MATCH_COUNT;

    // Note: the `FindMatchCache` struct and `mFindMatchCache` optional
    // both live on `mSession`; see `LogSession::FindMatchCache` and
    // `LogSession::FindMatchCacheState`. The alias below keeps the
    // legacy `FindMatchCache` name available inside `MainWindow`
    // during the incremental refactor.
    using FindMatchCache = LogSession::FindMatchCache;

    PreferencesEditor *mPreferencesEditor;

    /// Modeless editor for the merged regex-template catalog
    /// (`Settings -> Regex templates...`). Created lazily on first
    /// menu activation and reused across show/hide so in-flight
    /// edits survive a close-reopen. Parented to `this` (Qt-owned).
    /// Stays null when `mRegexTemplateRegistry` is null (the menu
    /// action is disabled in that case).
    RegexTemplatesEditor *mRegexTemplatesEditor = nullptr;

    /// Modeless editor for user-defined highlight rules
    /// (`Settings -> Highlight rules...`). Lazy-construct /
    /// survive-close, like the regex editor. `QPointer` so a
    /// teardown-time delete zeroes the slot even while queued
    /// signals from the rule set are in flight.
    QPointer<HighlightRulesEditor> mHighlightRulesEditor;
    /// Non-owning. Lives in `main()` (or the test fixture).
    /// `nullptr` for legacy no-args construction; theme code paths
    /// in this class check before dereferencing.
    ThemeControl *mTheme;

    /// Non-owning alias into `mSession->Anchors()` (task 2.1;
    /// review finding #11). The `AnchorManager` itself is owned
    /// by `mSession` and reaped in reverse order via
    /// `~LogSession()` so `~LogModel` runs while its non-owning
    /// back-pointer is still valid. Non-null after construction.
    /// See the model-quintet aliases block above for the full
    /// lifetime discussion and the caveat for consumer QObjects
    /// registered after `mSession`.
    AnchorManager *mAnchors = nullptr;

    /// Non-owning alias into `mSession->Highlights()` (task 2.1;
    /// review finding #11). Runtime companion to
    /// `LogConfiguration::highlightRules`. Constructed before
    /// `mModel` inside `LogSession`'s ctor initializer list so
    /// the model can hold a non-owning pointer for the paint
    /// cascade. Same lifetime story as `mAnchors`.
    HighlightRuleSet *mHighlights = nullptr;

    /// Owned. Hidden by default; toggled via View -> Anchors.
    AnchorsDock *mAnchorsDock = nullptr;

    /// Toggle action for the Anchors dock. Re-added to View on every
    /// `RebuildViewMenu`. Programmatic because the .ui has no entry.
    QAction *mActionToggleAnchors = nullptr;

    /// Owned. Bottom-docked histogram strip; hidden by default,
    /// toggled via View -> Histogram (or Ctrl+H).
    HistogramDock *mHistogramDock = nullptr;

    /// Toggle action for the Histogram dock; re-added on every
    /// `RebuildViewMenu`. Programmatic because the .ui has no entry.
    QAction *mActionToggleHistogram = nullptr;

    /// Non-owning alias into `mSessionView->OverviewRailModelPtr()`
    /// (task 3.3). The bucket model is a child of `mSessionView`,
    /// not the shell. Kept alive even when the rail is hidden so
    /// the toggle is instant.
    OverviewRailModel *mOverviewRailModel = nullptr;

    /// Non-owning alias into `mSessionView->OverviewRail()` (task
    /// 3.3). Constructed as a child of `mSessionView` so it dies
    /// with the tab.
    ///
    /// Visibility toggle preserves the pre-migration attach dance:
    /// when visible, `LogTableView::AttachOverviewRail` reparents
    /// the widget INTO the table view (it lives inside the table's
    /// reserved right viewport margin); when hidden,
    /// `SetOverviewRailVisible(false)` reparents it back onto
    /// `mSessionView` (post-review finding #3 -- previously
    /// reparented onto the shell, which orphaned the widget on
    /// tab close). The widget is intentionally NOT placed in
    /// `LogSessionView`'s `QVBoxLayout`; the layout stays a
    /// single-child stack of the table view. `QPointer` so a
    /// teardown-time delete zeroes the slot; the slot is null
    /// after `~LogSessionView` runs.
    QPointer<OverviewRailWidget> mOverviewRailWidget;

    /// Checkable toggle for the overview rail, mirrored onto the
    /// primary toolbar and the View menu. Persisted through
    /// `ui/showOverviewRail`.
    QAction *mActionToggleOverviewRail = nullptr;

    /// Anchor hotkey actions: index N maps to `Ctrl+(N+1)`.
    /// `mActionClearRowAnchor` is `Ctrl+0`; jumps are `F2` /
    /// `Shift+F2`; edit-note is `F4`; clear-all is `Ctrl+Shift+A`.
    /// Registered via `addAction` so they fire even without menu
    /// placement.
    std::array<QAction *, loglib::ANCHOR_PALETTE_SIZE> mAnchorColorActions{};
    QAction *mActionClearRowAnchor = nullptr;
    QAction *mActionJumpNextAnchor = nullptr;
    QAction *mActionJumpPrevAnchor = nullptr;
    QAction *mActionEditRowAnchorNote = nullptr;
    QAction *mActionClearAllAnchors = nullptr;
    /// Simple-mode leaves and their display order now live on
    /// `mSession` (task 2.4); MainWindow methods reach them
    /// through `mSession->SimpleLeaves()` /
    /// `mSession->MutableSimpleLeaves()` /
    /// `mSession->SimpleLeafOrder()` /
    /// `mSession->MutableSimpleLeafOrder()`. The public `Filters()`
    /// accessor keeps its shape for tests.

    /// Status-bar label shown while a streaming session is active.
    QLabel *mStatusLabel = nullptr;

    /// Status-bar label that reads "*n* of *m* shown" while a
    /// filter is hiding rows, or "*m* lines" otherwise. Hidden
    /// when the source model is empty. Updated via
    /// `UpdateRowsShownStatus` from source / proxy row signals.
    QLabel *mRowsShownLabel = nullptr;

    /// Status-bar button that triggers `actionClearAllFilters`.
    /// Visible only when at least one filter is active and the
    /// source model has rows. Mirrors the UX of
    /// `mDiagnosticsButton` / `mParseErrorsStatusButton`.
    QPushButton *mClearFiltersStatusButton = nullptr;

    /// Status-bar button bound to `actionClearSort`. Visible
    /// only while a sort is active and the source has rows.
    /// Same flat / clickable styling as
    /// `mClearFiltersStatusButton`.
    QPushButton *mClearSortStatusButton = nullptr;

    /// Status-bar button showing the per-column type-mismatch
    /// summary. Hidden when zero columns are mismatched; opens the
    /// diagnostics dialog on click.
    QPushButton *mDiagnosticsButton = nullptr;

    /// Lazy-owned diagnostics dialog; survives close so a second
    /// open reuses the same window.
    QPointer<class ConfigurationDiagnosticsDialog> mDiagnosticsDialog;

    /// The session that owned the model this diagnostics dialog is
    /// bound to (task 5.8). Captured when the dialog is (re)opened
    /// and consulted by `RebindSharedDocks` -- when the active
    /// session changes to a different one, we close the dialog
    /// rather than let it silently retain a stale model pointer.
    /// `QPointer` so a session torn down out-of-order zeroes the
    /// alias and the "same session" check degrades to false, which
    /// still closes the dialog cleanly.
    QPointer<LogSession> mDiagnosticsDialogSession;

    /// Lazy-owned bulk column manager dialog; survives close so a
    /// second open reuses the same window.
    QPointer<class ColumnsManagerDialog> mColumnsManagerDialog;

    /// Originating session for `mColumnsManagerDialog` (task 5.8);
    /// same semantics as `mDiagnosticsDialogSession` above.
    QPointer<LogSession> mColumnsManagerDialogSession;

    /// Originating session for `mHighlightRulesEditor` (task 5.9).
    /// Same semantics as `mDiagnosticsDialogSession` above --
    /// consulted by `RebindSharedDocks` to close the editor when
    /// the active session changes to a different one, so the
    /// `rulesSaved` fan cannot land on the wrong session's model.
    QPointer<LogSession> mHighlightRulesEditorSession;

    /// Dock pane that follows the selected row. Hidden until opened
    /// via the View menu or a double-click. `QDockWidget` provides
    /// the float / dock / close chrome.
    RecordDetailDock *mRecordDetailDock = nullptr;

    /// Last QSS pushed to the table body / header. Compared on
    /// re-apply so we can skip unchanged writes -- Qt re-polishes
    /// the whole view on every `setStyleSheet`, even no-op ones.
    /// We cache our own snapshot (not `widget->styleSheet()`) so
    /// external writers can't trip the diff.
    QString mLastBodyStyleSheet;
    QString mLastHeaderStyleSheet;

    /// One snapshot window plus the scoped `destroyed` connection
    /// installed by `OpenRecordDetailWindow`. The scoped handle lets
    /// `~MainWindow` disconnect only what we wired (a blanket
    /// `disconnect` would catch unrelated future hooks).
    struct TrackedSnapshotWindow
    {
        QPointer<RecordDetailWindow> window;
        QMetaObject::Connection destroyedConnection;
    };

    /// Open snapshot windows keyed by the original heap address (cast
    /// to `quintptr` for stable identity). Each window is
    /// `Qt::WA_DeleteOnClose`; the `destroyed` lambda removes the
    /// entry by id, so the map self-compacts without sweeps and
    /// removal is unambiguous under concurrent teardown.
    QHash<quintptr, TrackedSnapshotWindow> mRecordDetailWindows;

    /// Toolbar holding Pause/Follow tail/Stop; visible only during a
    /// live-tail session.
    QToolBar *mStreamToolbar = nullptr;

    /// Persistent primary toolbar (Open / Filter / View toggles /
    /// Preferences). Inserted ahead of `mStreamToolbar` in the top
    /// dock area, so the combined strip reads "Main | Stream"
    /// left-to-right when both are visible. `QPointer` because
    /// `RefreshThemedIcons` can be invoked during shutdown after
    /// Qt has begun tearing down child widgets but before the
    /// `MainWindow` destructor finishes; a dangling raw pointer
    /// would crash on the next palette change. `objectName` is
    /// `mainToolbar` so `restoreState` round-trips its dock area
    /// and visibility.
    QPointer<QToolBar> mMainToolbar;

    /// One themed action plus the widget that drives its tinting
    /// policy (palette / iconSize / DPR). Used by
    /// `RefreshThemedIcons` as the single registry of "actions
    /// that need re-tinting on palette / theme / DPR change".
    ///
    /// The anchor is a hint, not an ownership relation: most
    /// toolbar actions point at their host toolbar so the pixmap
    /// is rasterised at the toolbar's exact `iconSize` (avoiding
    /// downsample on platforms whose style reports a larger
    /// `PM_LargeIconSize`). Actions reached only via menus
    /// (`menuRecentSessions`) point at the window because there
    /// is no toolbar to anchor against.
    ///
    /// Both fields are `QPointer` so an action / widget torn down
    /// out of order during shutdown surfaces as null instead of
    /// dangling.
    struct ThemedActionEntry
    {
        QPointer<QAction> action;
        QPointer<QWidget> anchor;
    };

    /// Every action whose icon is generated by `icon_loader` and
    /// needs re-tinting on palette / theme / DPR change.
    /// Populated once by `BuildMainToolbar`; cleared on rebuild
    /// for idempotency. Includes:
    ///   * Main-toolbar actions (anchor = `mMainToolbar`).
    ///   * Stream-toolbar actions (anchor = `mStreamToolbar`).
    ///   * Open-stream split button's default + dropdown actions
    ///     (anchor = `mMainToolbar`) -- these would be missed by
    ///     a toolbar-iteration refresh because `addWidget` wraps
    ///     the button in an internal `QWidgetAction` and the
    ///     underlying action is not in `toolbar->actions()`.
    ///   * `File -> Recent Sessions` submenu indicator (anchor =
    ///     `this`) and any other future menu-only themed action.
    QList<ThemedActionEntry> mThemedActions;

    /// Ctrl+/ action that opens the shortcuts reference dialog.
    QAction *mActionShowShortcuts = nullptr;

    /// Lazy-built shortcuts dialog; kept alive so reopening preserves geometry.
    QPointer<class ShortcutsDialog> mShortcutsDialog;

    // Note: the wall-clock ``QElapsedTimer`` since the active
    // live-tail session started lives on `mSession`; see
    // `LogSession::LiveTailElapsedTimer`. The 1 Hz UI tick timer
    // below stays on the shell because rendering is a view concern.

    /// 1 Hz timer that refreshes the live-tail elapsed-time display.
    QTimer *mLiveTailTickTimer = nullptr;

    /// The "filters dirty" marker (`[*]` in the window title) and the
    /// re-entrancy gate the configuration-load path uses to coalesce
    /// per-filter mutations now live on `mSession` (task 2.4). The
    /// window subscribes to `LogSession::filtersDirtyChanged` and
    /// projects the value into `setWindowModified` from
    /// `UpdateWindowTitle`.

    /// The filename of the active stream lives on `mSession`
    /// (task 2.5). Reach it through `mSession->StreamingFileName()`
    /// / `mSession->SetStreamingFileName()` /
    /// `mSession->ClearStreamingFileName()`.

    /// Source descriptor lives on `mSession` (task 2.5). Reach it
    /// through `mSession->CurrentSource()` /
    /// `mSession->MutableCurrentSource()` /
    /// `mSession->SetCurrentSource()` /
    /// `mSession->ResetCurrentSource()`. Mirrored into
    /// `LogConfiguration::source` before a `SaveScope::Full` save by
    /// `MirrorSessionStateToConfiguration`.

    /// Non-owning. Provided by `main()` for the production window;
    /// `nullptr` for ad-hoc / test-only instances, in which case
    /// auto-save / Recent Sessions / restore-on-launch are no-ops.
    SessionHistoryManager *mHistoryManager = nullptr;

    /// Non-owning. Provided by `main()` so `OpenNetworkStream` can
    /// pass it to `NetworkStreamDialog`. `nullptr` for ad-hoc
    /// instances, in which case the dialog uses the library's
    /// built-in template catalog only.
    RegexTemplateRegistry *mRegexTemplateRegistry = nullptr;

    // Note: `mAutoSaveUuid` (recents-entry uuid pinned to this
    // session) and `mAutoSaveUuidPublished` (publish latch mirror
    // for the process-shared `openWindowsAtQuit` set) live on
    // `mSession`; see `LogSession::AutoSaveUuid` and
    // `LogSession::IsAutoSaveUuidPublished`.

    // Note: `mPendingOpenFiles` (the FIFO of queued static files
    // populated by `StartStreamingOpenQueue`) lives on `mSession`;
    // see `LogSession::PendingOpenFiles`.
    //
    // Note: `mPendingLiveTailPrimary` / `mPendingLiveTailRetention`
    // (the static-prefix-to-live-tail promotion pair),
    // `mDisableRotationHistoryOverride` (per-window CLI opt-out),
    // and `mLastRotationExpansion{OriginalInputs,WasLiveTail}` (undo
    // capture) all live on `mSession`; see
    // `LogSession::PendingLiveTailPrimary`,
    // `LogSession::DisableRotationHistoryOverride`, and
    // `LogSession::LastRotationExpansionOriginalInputs`.
    //
    // Note: `mPendingOpenErrors` (parse/open errors accumulated
    // during a multi-file drain, drained under
    // `tr("Error Opening File")`) and `mPendingDecompressionErrors`
    // (decompression-specific errors drained under
    // `tr("Error Decompressing File")`) live on `mSession`; see
    // `LogSession::PendingOpenErrors` /
    // `LogSession::PendingDecompressionErrors`.

    /// QFutureWatcher for the current async decompression. Owns a
    /// `std::shared_ptr<DecompressingByteSource>`; the shared_ptr
    /// is captured into the subsequent parse callable so the temp
    /// file survives for the whole parse. `nullptr` when no
    /// decompression is in flight.
    // Note: the QFutureWatcher itself lives on `mSession` (task 2.8);
    // see `LogSession::DecompressionWatcherPtr`. The shell still owns
    // the connection to `OnDecompressionFinished` because the slot
    // body operates on widgets.

    // Note: `mDecompressionInFlight` (latch guarding the finished
    // slot against stale queued callouts) and `mDecompressionStopSource`
    // (cooperative-cancel source refreshed per open) live on
    // `mSession`; see `LogSession::IsDecompressionInFlight` and
    // `LogSession::DecompressionStopSource`.

    // Note: the decompression progress atomics (bytes-in and total
    // compressed size) live on `mSession`; see
    // `LogSession::DecompressionBytesIn` and
    // `LogSession::DecompressionTotalBytesIn`. The `mDecompressionPollTimer`
    // still lives here because rendering is a shell concern.

    /// 200 ms cadence timer that pumps the atomics above into the
    /// progress dialog. Nulled out when no decompression is
    /// active.
    QTimer *mDecompressionPollTimer = nullptr;

    /// Modal-per-window progress dialog surfaced while a
    /// decompression is running. `QPointer` because `deleteLater`
    /// may run between the finished slot and the parent's
    /// destructor.
    QPointer<QProgressDialog> mDecompressionProgressDialog;

    /// Origin binding for the decompression poll timer (review
    /// finding #2). Captured at `BeginAsyncDecompression` time so a
    /// timer tick that fires after a session/tab swap does not
    /// paint on a sibling session's view. `QPointer` clears
    /// automatically on session/view teardown; a null pointer is
    /// the "operation no longer belongs to a live session" branch.
    QPointer<LogSession> mDecompressionPollOriginSession;
    QPointer<LogSessionView> mDecompressionPollOriginView;

    /// Operation generation captured at `BeginAsyncDecompression`
    /// time (review finding #4). Compared against
    /// `mDecompressionPollOriginSession->DecompressionGeneration()`
    /// on each tick; a mismatch means a queued completion already
    /// drained into the next-queued file and this tick must not
    /// scribble stale text into the successor's dialog.
    std::uint64_t mDecompressionPollGeneration = 0;

    // Note: `mDecompressionOriginalPath` (user-facing path being
    // decompressed), `mDecompressionCodecName` (pre-sniffed codec
    // label rendered by the poll-timer lambda), and
    // `mDecompressionStartedAt` (wall-clock start for the completion
    // toast) all live on `mSession`; see
    // `LogSession::DecompressionOriginalPath`.

    // --------------------------- Filtered-row export -----------------
    // Async orchestration for `File -> Export Filtered Rows...`.
    // Mirrors the decompression block above: fresh `StopSource` per
    // run, own `QFutureWatcher<void>`, modal-per-window
    // `QProgressDialog` with `minimumDuration`-deferred show, atomic
    // progress counter polled by a `QTimer`.

    // Note: the export QFutureWatcher itself lives on `mSession`
    // (task 2.9); see `LogSession::ExportWatcherPtr`. The shell still
    // owns the connection to `OnExportFinished` because the slot
    // body operates on widgets.

    // Note: `mExportInFlight` (finished-slot guard) and
    // `mExportStopSource` (cooperative-cancel source refreshed per
    // export) live on `mSession`; see `LogSession::IsExportInFlight`
    // and `LogSession::ExportStopSource`.

    // Note: the export progress atomics (rows written / rows total)
    // live on `mSession`; see `LogSession::ExportRowsWritten` and
    // `LogSession::ExportRowsTotal`. The `mExportPollTimer` still
    // lives here because rendering is a shell concern.

    /// 200 ms poll timer that pumps the atomics into the progress
    /// dialog.
    QTimer *mExportPollTimer = nullptr;

    /// Modal-per-window progress dialog. `QPointer` because
    /// `deleteLater` may run between the finished slot and
    /// destruction.
    QPointer<QProgressDialog> mExportProgressDialog;

    /// Origin binding for the export poll timer. Same semantics as
    /// the decompression counterpart above (review finding #2, #4).
    QPointer<LogSession> mExportPollOriginSession;
    QPointer<LogSessionView> mExportPollOriginView;
    std::uint64_t mExportPollGeneration = 0;

    // Note: `mExportDestinationPath` (user-facing destination),
    // `mExportFormatLabel` (human-readable format label), and
    // `mExportStartedAt` (wall-clock start for the toast) all live
    // on `mSession`; see `LogSession::ExportDestinationPath`.

    // Note: `mApplyEmbeddedBundleConfigForPath` (bundle path allowed
    // to apply embedded configuration) lives on `mSession`; see
    // `LogSession::ApplyEmbeddedBundleConfigForPath`.

    /// Kick off the async export worker. Models on
    /// `BeginAsyncDecompression`.
    void BeginAsyncExport(
        std::unique_ptr<slv::exports::ExportPlan> plan, const QString &destination, const QString &formatLabel
    );

    /// Kick off the async bundle-write worker. Mirrors
    /// `BeginAsyncExport`: same in-flight guard and progress/cancel
    /// plumbing, but the payload is `WriteSessionBundle` rather
    /// than `RowExporter::Run`.
    void BeginAsyncBundleExport(std::filesystem::path destination, int compressionLevel, int totalWorkers);

    /// Show the export progress dialog (deferred via
    /// `minimumDuration`).
    void ShowExportProgress();

    /// Reset dialog + poll timer without deleting them (reused
    /// across runs).
    void TeardownExportProgress();

    /// Handle worker completion (success / cancel / error) and
    /// drive the user-facing toast or message.
    void OnExportFinished();

    /// Cancel any in-flight export and reset scratch state. Safe
    /// to call when nothing is running.
    void CancelInFlightExport();

    // --------------------------- Session mode ------------------------

    /// `SessionMode` is aliased near the top of the class to
    /// `LogSession::Mode` (task 2.5). `mSessionMode` /
    /// `mLastTerminalSessionMode` used to live on this class as
    /// plain members; both fields now live on `mSession` and are
    /// reached through `mSession->SessionMode()` /
    /// `mSession->LastTerminalMode()` / `mSession->SetMode()`.

    // Note: `mExportIsBundle` (bundle vs. plain-export label
    // selector) lives on `mSession`; see `LogSession::IsExportBundle`.

    [[nodiscard]] bool IsSessionActive() const noexcept
    {
        return mSession->IsSessionActive();
    }
    [[nodiscard]] bool IsLiveTailSession() const noexcept
    {
        return mSession->IsLiveTailSession();
    }

    /// Streaming progress counters (`mStreamingLineCount`,
    /// `mStreamingErrorCount`, `mStreamingErrorsCut`,
    /// `mFirstStreamingBatchSeen`) and the `SourceStatus::Waiting`
    /// latch (`mSourceWaiting`) live on `mSession` (task 2.5). Reach
    /// them through `mSession->StreamingLineCount()` /
    /// `SetStreamingLineCount()`, `StreamingErrorCount()` /
    /// `SetStreamingErrorCount()`, `StreamingErrorsCut()` /
    /// `SetStreamingErrorsCut()`, `FirstStreamingBatchSeen()` /
    /// `SetFirstStreamingBatchSeen()`, and `IsSourceWaiting()` /
    /// `SetSourceWaiting()`. `mSession->ResetStreamingProgress()`
    /// covers the per-file start pattern (`line = error = 0;
    /// firstBatchSeen = false`); `ResetStreamingCountersAndFileName()`
    /// clears every field including the file name.

    // Note: `mRotationFlashActive` moved to `LogSession` in the
    // phase-4 review-4 resolution (finding #4). The flash state
    // is per-session so multi-tab windows never project one
    // tab's flash onto another; the timer that clears it is now
    // owned by `LogSession` as well, receiver-bound to the
    // session so teardown safely cancels the pending clear. See
    // `LogSession::TriggerRotationFlash()` /
    // `IsRotationFlashActive()`.

    /// Re-entrancy guard for `OnHeaderSectionMoved`: the slot
    /// re-fires `sectionMoved` while resetting visual order, and
    /// we swallow that volley.
    bool mApplyingSectionMove = false;

    /// Re-entrancy guard for `enumColumnsChanged -> UpdateFilters`
    /// now lives on `mSession` (task 2.4). Reach it through
    /// `mSession->IsApplyingEnumRebuild()` /
    /// `mSession->SetApplyingEnumRebuild()`.

    // Note: `mLastGotoTimestampInput` and `mLastGotoLineInput`
    // migrated to `LogSessionView` (task 3.6). Sticky-input state
    // is a view concern; the shell's session-switch path clears
    // both through `mSessionView->ClearGotoStickyInputs()` and
    // the test seam `LastGotoTimestampInputForTest` forwards to
    // the view.

    /// The pending-apply-sort-from-config latch (see the deferred
    /// sort block in `LogSession`) now lives on `mSession` (task
    /// 2.4). Existing MainWindow call sites use
    /// `mSession->HasPendingApplySortFromConfig()` /
    /// `mSession->SetPendingApplySortFromConfig()`.

    /// Latch held by the `SessionSwitchScope` RAII helper across a
    /// destructive `mModel->Reset()` now lives on `mSession`
    /// (task 2.5). Reach it through
    /// `mSession->IsSessionSwitchInProgress()` /
    /// `mSession->SetSessionSwitchInProgress()`.

#ifdef LOGAPP_BUILD_TESTING
    /// Skip `ShowDroppedFiltersDialog`'s modal so a headless test
    /// thread is not blocked.
    bool mSuppressDialogsForTest = false;
    int mLastDroppedFilterCountForTest = 0;
#endif
};

template <typename DockT>
void MainWindow::WireDockToggle(
    DockT *dock,
    QAction *action,
    void (DockT::*closedSignal)(),
    const std::function<void()> &onShow,
    const std::function<void()> &onShown
)
{
    Q_ASSERT(dock != nullptr);
    Q_ASSERT(action != nullptr);
    // Toggle -> show / close. Reverts the toggle when the host
    // isn't realised (early test driver) so the action can't sit
    // checked while the dock stays hidden.
    connect(action, &QAction::toggled, this, [this, dock, action, onShow](bool on) {
        if (on && !isVisible())
        {
            const QSignalBlocker blocker(action);
            action->setChecked(false);
            return;
        }
        if (on)
        {
            if (onShow)
            {
                onShow();
            }
            else
            {
                dock->show();
                dock->raise();
            }
        }
        else
        {
            // `close()` (not `hide()`) so `closeEvent` fires and
            // `closedSignal` propagates.
            dock->close();
        }
    });
    // `visibilityChanged(true)` fires for cold reveals AND tab
    // activations; both want the action checked. We don't listen
    // to the false edge -- `closedSignal` handles that side because
    // tab inactivation also fires `visibilityChanged(false)`.
    connect(dock, &QDockWidget::visibilityChanged, this, [action, onShown](bool visible) {
        if (!visible)
        {
            return;
        }
        const QSignalBlocker blocker(action);
        action->setChecked(true);
        if (onShown)
        {
            onShown();
        }
    });
    connect(dock, closedSignal, this, [action]() {
        const QSignalBlocker blocker(action);
        action->setChecked(false);
    });
}
