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
#include "workspace_persistence.hpp"

#include <QUuid>

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
class QTabWidget;
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
    /** @brief Alias for the session mode owned by `LogSession`. */
    using SessionMode = LogSession::Mode;

    /**
     * @brief Selects how `StartStreamingOpenQueue` interacts with the
     * current state. `Append` queues new files onto the active
     * static session without clobbering its filters / sort / rows.
     * `Replace` resets the model, clears filters, and drops the
     * source descriptor first. Live-tail / network sessions are
     * single-source and always behave as `Replace`.
     */
    enum class OpenMode
    {
        Append,
        Replace,
    };

    /**
     * @brief Outcome of `DispatchMixedOpenInput`. Lets callers attach
     * entry-point-specific tails (e.g. the CLI `AppliedConfigOnly`
     * status-bar hint) based on which branch the dispatcher took.
     */
    enum class MixedInputDispatch
    {
        /**
         * @brief No configs in the input -- streamed via
         * `StartStreamingOpenQueue` in the caller's `OpenMode`.
         */
        QueuedLogsOnly,
        /**
         * @brief One config, no logs -- applied via `TryLoadAsConfiguration`
         * (no model reset; existing rows survive).
         */
        AppliedConfigOnly,
        /**
         * @brief One config + N logs -- applied via `DoLoadConfiguration`
         * (full reset), then logs streamed in `Append` mode so the
         * freshly-loaded columns / filters / sort apply.
         */
        AppliedConfigThenLogs,
        /**
         * @brief Two or more configs -- rejected with a warning dialog and
         * no state mutated.
         */
        RejectedMultiConfig,
    };

    /**
     * @brief Full result of `DispatchMixedOpenInput`. `appliedConfigPath`
     * is non-empty iff `outcome` is `AppliedConfigOnly` or
     * `AppliedConfigThenLogs`. Threading the chosen path back to
     * the caller lets user-facing status messages name the actual
     * configuration argument (not `files.front()`, which silently
     * lies when the config is not the first positional).
     */
    struct MixedInputResult
    {
        MixedInputDispatch outcome = MixedInputDispatch::QueuedLogsOnly;
        QString appliedConfigPath;
    };

    /**
     * @brief No-history, no-theme constructor: auto-save / Recent
     * Sessions / restore-on-launch are all no-ops, and the table
     * renders without per-level styling. Used by
     * `MainWindow mainWindow;` test sites that don't exercise
     * the theme system; pair the test fixture with a real
     * `ThemeControl` via the themed overload for theme-aware
     * assertions.
     * @param parent The optional Qt parent.
     */
    MainWindow(QWidget *parent = nullptr);

    /**
     * @brief Themed, no-history constructor for test fixtures and
     * ad-hoc instances that need a live theme but don't care
     * about session history.
     * @param theme The `theme` value.
     * @param parent The optional Qt parent.
     */
    MainWindow(ThemeControl *theme, QWidget *parent = nullptr);

    /**
     * @brief Production constructor. The theme controller, history
     * manager and regex-template registry are owned by `main()`;
     * the window keeps non-owning pointers and writes snapshots
     * through the history manager on streaming completion /
     * close. Any of `theme` / `regexTemplateRegistry` may be
     * nullptr in tests; theme code paths fall back to defaults,
     * and the network-stream dialog falls back to the library's
     * built-in template catalog.
     *
     * No separate `(theme, history, parent)` overload exists on
     * purpose: it would make `MainWindow(theme, history, nullptr)`
     * test calls ambiguous with this one. Tests that don't need a
     * registry keep their 3-arg shape and resolve here with
     * `parent` defaulted.
     * @param theme The `theme` value.
     * @param historyManager The `historyManager` value.
     * @param regexTemplateRegistry The `regexTemplateRegistry` value.
     * @param parent The optional Qt parent.
     */
    MainWindow(
        ThemeControl *theme,
        SessionHistoryManager *historyManager,
        RegexTemplateRegistry *regexTemplateRegistry,
        QWidget *parent = nullptr
    );

    ~MainWindow();

    /**
     * @brief Locate the staged `tzdata/` directory and initialise loglib's
     * timezone database from it. Idempotent.
     *
     * Must be called before any timestamp-formatting code path.
     * `main()` calls this before constructing the primary window
     * and before the restore-on-launch flow; the test fixture
     * mirrors the call in `initTestCase`. Without this ordering
     * the first `loglib::CurrentZone()` (triggered by loading a
     * session with a time-range filter) probes the date library's
     * platform default path (on Windows: `<profile>/Downloads/tzdata`)
     * and fails with a misleading "Error Parsing Configuration".
     *
     * Returns true on success. On failure logs a `qCritical`
     * diagnostic and returns false; `main()` propagates that as a
     * non-zero exit code.
     * @return The result described above.
     */
    [[nodiscard]] static bool InitializeTimezoneDatabase();

    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

    /**
     * @brief Restore the auto-saved session at @p jsonPath. Same logic as
     * the Recent Sessions reopen path, but starts from a JSON path
     * so it can run before any menu wiring (used by `main()`'s
     * restore-on-launch flow).
     *
     * `mAutoSaveUuid` is pinned only when the stem parses as a
     * `QUuid` AND @p jsonPath lives in `mHistoryManager->SessionsDir()`.
     * For external / non-uuid JSONs the configuration loads but the
     * pin is skipped: pinning would let the next AutoSave write a
     * managed copy under that stem and silently fork the user's
     * original file. External JSONs stay read-only in place; the
     * next save mints a fresh uuid.
     * @param jsonPath The `jsonPath` value.
     */
    void RestoreLastSessionFromPath(const QString &jsonPath);

    /**
     * @brief Open CLI-provided file paths. Behaves like `OpenFiles` but
     * bypasses the dialog; used by `main()` after parsing argv and
     * by the single-instance forward handler. Always Append mode
     * so pre-loaded configuration filters survive into the new
     * session.
     * @param files The file paths to process.
     */
    void OpenFilesForCli(const QStringList &files);

    /**
     * @brief Set this window's CLI rotation-history opt-out. It overrides
     * global and session preferences until the user toggles the
     * corresponding Settings action.
     * @param disable The `disable` value.
     */
    void SetRotationHistoryLaunchOverride(bool disable);

    /**
     * @brief Test-only reader for this window's CLI opt-out.
     * @return The result described above.
     */
    [[nodiscard]] bool RotationHistoryLaunchOverrideForTest() const noexcept
    {
        return mSession->DisableRotationHistoryOverride();
    }

    /**
     * @brief Test-only entry to the rotation-history Settings handler.
     * @param enabled The new enabled state.
     */
    void SimulateRotationHistoryMenuToggleForTest(bool enabled)
    {
        OnRotationHistoryPrefToggled(enabled);
    }

    /**
     * @brief Returns the number of active-session-scoped connections.
     * @return The current scoped connection count.
     */
    [[nodiscard]] std::size_t SessionConnectionCountForTest() const noexcept
    {
        return mSessionConnections.Size();
    }

    /**
     * @brief Returns the number of persistent connections stored on a tab.
     * @param index The tab index to inspect.
     * @return The connection count, or zero when the index is not hosted.
     */
    [[nodiscard]] std::size_t PerTabConnectionCountForTest(int index) const noexcept;

    /**
     * @brief Open a live-tail session over the process's standard input.
     * The CLI parses `-` / `--stdin` in argv and routes here via
     * `main()`. Session shape mirrors `OpenNetworkStream` (live-
     * tail, `Kind::Stdin`, excluded from Recent Sessions): stdin is
     * one-shot per process, so there's nothing to reopen on restart.
     */
    void OpenStdinStream();

    /**
     * @brief Shared implementation for `OpenStdinStream` and its test
     * seam. Takes ownership of the producer and its pre-read bytes.
     * @param producer The `producer` value.
     * @param peek The `peek` value.
     */
    void OpenStdinStreamFromProducer(std::unique_ptr<loglib::BytesProducer> producer, std::string peek);

    /**
     * @brief The auto-save uuid pinned to this window, or empty if none.
     * Used by `main()`'s `aboutToQuit` snapshot.
     * @return The result described above.
     */
    [[nodiscard]] QString ActiveSessionUuid() const noexcept
    {
        return mSession->AutoSaveUuid();
    }

    /**
     * @brief Returns the session hosted by the active tab.
     *
     * The returned pointer is non-owning and valid only while its tab remains
     * hosted. Resolve it when handling an action; asynchronous work must retain
     * a `QPointer<LogSession>` for the originating session.
     *
     * @return The active session, non-null after construction.
     */
    [[nodiscard]] LogSession *activeSession() const noexcept
    {
        return mSession;
    }

    /**
     * @brief Returns the view hosted by the active tab.
     * @return The non-owning active view pointer, or `nullptr` during teardown.
     */
    [[nodiscard]] LogSessionView *activeSessionView() const noexcept
    {
        return mSessionView;
    }

    /**
     * @brief Builds a binding context for the active session and view.
     * @return A bound context, or an unbound context during teardown.
     */
    [[nodiscard]] SessionBindContext activeSessionBindContext() const;

    /**
     * @brief Returns all sessions hosted by this window in tab order.
     *
     * The pointers are non-owning and must not outlive their tabs.
     *
     * @return The currently hosted sessions.
     */
    [[nodiscard]] std::vector<LogSession *> hostedSessions() const;

    // Tab accessors used by window routing and tests.

    /**
     * @brief Returns the number of open tabs.
     * @return The tab count, or zero before tab setup or during teardown.
     */
    [[nodiscard]] int TabCount() const noexcept;

    /**
     * @brief Returns the active tab index.
     * @return The current index, or `-1` when the tab widget is unavailable.
     */
    [[nodiscard]] int ActiveTabIndex() const noexcept;

    /**
     * @brief Exposes the central tab widget for tests.
     * @return The non-owning tab-widget pointer, or `nullptr` outside its lifetime.
     */
    [[nodiscard]] QTabWidget *TabWidgetForTest() const noexcept
    {
        return mTabWidget;
    }

    /**
     * @brief Returns the session hosted at a tab index.
     * @param index The tab index to inspect.
     * @return A non-owning session pointer, or `nullptr` when unavailable.
     */
    [[nodiscard]] LogSession *SessionAtTab(int index) const noexcept;

    /**
     * @brief Finds the tab hosting a session instance.
     * @param id The stable session instance identifier.
     * @return The current tab index, or `-1` when no tab matches.
     */
    [[nodiscard]] int TabIndexForSession(SessionInstanceId id) const noexcept;

    /**
     * @brief Returns the hosted session for a stable instance identifier.
     * @param id The session instance identifier to resolve.
     * @return The hosted session, or `nullptr` when no tab currently owns that id.
     */
    [[nodiscard]] LogSession *HostedSession(SessionInstanceId id) const noexcept;

    /**
     * @brief Returns the view hosted at a tab index.
     * @param index The tab index to inspect.
     * @return A non-owning view pointer, or `nullptr` when unavailable.
     */
    [[nodiscard]] LogSessionView *ViewAtTab(int index) const noexcept;

    /**
     * @brief Adds an empty tab through the production tab-creation path.
     * @param makeActive Whether to activate the new tab.
     * @return The new session identifier, or an invalid identifier if tab setup is unavailable.
     */
    SessionInstanceId AddNewTabForTest(bool makeActive = true);

    /**
     * @brief Activates a tab for tests.
     * @param index The tab index; invalid indices are ignored.
     */
    void ActivateTabForTest(int index);

    /**
     * @brief Closes a tab through the production close path.
     * @param index The tab index; invalid indices are ignored.
     *
     * Closing the final tab closes the window. Cancellation in the close-decision
     * prompt leaves the tab unchanged.
     */
    void CloseTabForTest(int index);

    /**
     * @brief Informative text shown for a Save / Discard / Cancel close prompt.
     * @param session Session whose source and dirty flags shape the copy.
     * @return Localized explanatory text that does not mention session-bundle export.
     */
    [[nodiscard]] static QString ClosePromptInformativeTextForTest(const LogSession &session);

    /**
     * @brief Disconnects all active-session-scoped subscriptions for tests.
     *
     * This operation is idempotent and does not clear session or view aliases.
     * Production tab switches reinstall the bag through
     * `InstallActiveSessionConnections()`.
     */
    void UnbindActiveSessionForTest() noexcept;

    /**
     * @brief Rebinds shared docks and session-scoped dialogs in dependency order.
     * @param context The incoming session context, or an unbound context to clear state.
     *
     * Session-specific dialogs whose origin differs from the context are closed
     * and scheduled for deletion.
     */
    void RebindSharedDocks(const SessionBindContext &context);

    /** @brief Updates the window-modified marker from all hosted sessions. */
    void AggregateWindowModified();

    /**
     * @brief Applies the global rotation-history preference to every hosted session.
     * @param enabled The new global preference value.
     */
    void BroadcastRotationHistoryPreference(bool enabled);

    /**
     * @brief Like `ActiveSessionUuid`, but returns empty when the current
     * session cannot be fan-restored on next launch (no source,
     * network stream, ...). `main()`'s `aboutToQuit` handler uses
     * this to avoid publishing non-restorable uuids into
     * `openWindowsAtQuit` (which would otherwise loop the user on
     * the "Network Stream Session" info popup every launch).
     * @return The result described above.
     */
    [[nodiscard]] QString RestorableActiveSessionUuid() const noexcept;

    /**
     * @brief Collects restorable session identifiers from all hosted tabs.
     * @return Restorable UUIDs in tab order; non-restorable sessions are omitted.
     */
    [[nodiscard]] QStringList RestorableHostedSessionUuids() const;

    /**
     * @brief Auto-saves every hosted session.
     * @param publishOpenWindow Whether each saved UUID is published for launch restoration.
     *
     * The operation temporarily activates tabs as required by alias-based save
     * helpers and restores the original active tab. Repeated calls are safe.
     */
    void AutoSaveAllHostedSessions(bool publishOpenWindow);

    /**
     * @brief Captures geometry, dock state, tab order, and per-tab restore metadata.
     * @return A workspace record for this window with a valid active-tab index.
     */
    [[nodiscard]] slv::persistence::WorkspaceWindow CaptureWorkspaceWindow() const;

    /**
     * @brief Returns the stable identity used for workspace persistence and routing.
     * @return The existing UUID, or a newly generated UUID on first access.
     */
    [[nodiscard]] QString WorkspaceWindowUuid() const;

    /**
     * @brief Restores this window from persisted workspace state.
     * @param window The window record to apply.
     *
     * Restorable file-backed sessions reopen by UUID. Stream sessions become
     * disconnected placeholders, skipped entries remain empty, and individual
     * tab failures do not abort the remaining restore. Geometry and dock state
     * are applied after tab binding.
     */
    void ApplyWorkspaceWindow(const slv::persistence::WorkspaceWindow &window);

    /**
     * @brief Ensures a destructive open targets an empty active tab.
     *
     * A tab with a source or retained rows is preserved by creating and
     * activating a new tab; an already empty tab is reused.
     */
    void EnsureFreshActiveTab();

    /**
     * @brief Applies the session close-decision model to @p closing.
     *
     * Clean sessions proceed. Restorable file-backed sessions autosave
     * silently. Other dirty sessions offer Save, Discard, and Cancel.
     * A failed autosave or explicit save keeps the session, including
     * its autosave identity, and reports an error. Tests that suppress
     * dialogs treat a prompt as Discard unless a queued prompt choice
     * is pending.
     *
     * @param closing The session being closed or replaced; `nullptr` proceeds.
     * @return `true` to continue, or `false` when the user cancels or save fails.
     */
    [[nodiscard]] bool PrepareSessionClose(LogSession *closing);

    /**
     * @brief Offers Save, Discard, and Cancel for a dirty session that cannot
     * be autosaved.
     * @param closing Session identified in the prompt; must not be null.
     * @return `true` when Save succeeds or the user chooses Discard.
     */
    [[nodiscard]] bool PromptSaveDiscardCancel(LogSession *closing);

    /**
     * @brief Builds the close-prompt informative text for @p session.
     * @param session Session whose source and dirty flags shape the copy.
     * @return Localized explanatory text.
     */
    [[nodiscard]] static QString ClosePromptInformativeText(const LogSession &session);

private:
    /**
     * @brief Installs subscriptions that remain active while a tab is backgrounded.
     * @param session The hosted session; `nullptr` is ignored.
     *
     * Connections are stored on that session's `WindowTab` and are disconnected
     * before the tab is removed from the hosted registry.
     */
    void InstallPerTabPersistentConnections(LogSession *session);

    /**
     * @brief Completes streaming work against the session that started it.
     * @param origin The originating session; `nullptr` is ignored.
     * @param result The terminal streaming result.
     *
     * No-ops when `origin` is not currently hosted. Hosted background completion
     * rebinds aliases to the origin, runs the completion pipeline, and restores
     * the active tab.
     */
    void HandleStreamingFinishedFor(LogSession *origin, StreamingResult result);

public:
    /**
     * @brief Finds the hosted session that owns a decompression watcher.
     * @param watcherSender The watcher object reported by the completion signal.
     * @return The owning session, or `nullptr` when it is not currently hosted.
     */
    [[nodiscard]] LogSession *LogSessionForDecompressionWatcher(const QObject *watcherSender) const;

    /**
     * @brief Finds the hosted session that owns an export watcher.
     * @param watcherSender The watcher object reported by the completion signal.
     * @return The owning session, or `nullptr` when it is not currently hosted.
     */
    [[nodiscard]] LogSession *LogSessionForExportWatcher(const QObject *watcherSender) const;

private:
    /**
     * @brief Finds the hosted view associated with a session.
     * @param session The session to locate.
     * @return The non-owning view pointer, or `nullptr` when unavailable.
     */
    [[nodiscard]] LogSessionView *LogSessionViewForSession(const LogSession *session) const;

public:
    /**
     * @brief Mirror runtime session state into the configuration manager,
     * then `WriteSnapshot` through the injected history manager.
     * Reuses `mAutoSaveUuid` so a single window updates one recents
     * entry across its lifetime. Returns success when the manager is
     * null or there is no restorable source, because nothing needed
     * writing.
     *
     * When @p publishOpenWindow is true (the default), adds
     * `mAutoSaveUuid` to `openWindowsAtQuit` so a crash between
     * AutoSave and `closeEvent` still restores this window. The
     * `closeEvent` flush passes false because it immediately
     * removes the uuid again.
     *
     * Public so `main()`'s `aboutToQuit` handler can flush every
     * live window before exit.
     * @param publishOpenWindow The `publishOpenWindow` value.
     * @return `true` when a snapshot was written or none was required.
     */
    bool AutoSaveSessionSnapshot(bool publishOpenWindow = true);

    void UpdateUi();

    /**
     * @brief Single sync point for newest-first display: picks the right
     * `StreamingControl` flag for the active session mode and
     * propagates it to the proxy, table view, and row colours.
     * Idempotent.
     */
    void ApplyDisplayOrder();

    /**
     * @brief Test-only direct accessor for the live filter proxy. Tests
     * inspect the filtered row count and column-rank state through
     * it; production wires the proxy into `mTableView` directly.
     * @return The result described above.
     */
    [[nodiscard]] LogFilterModel *FilterModel() const
    {
        return mSortFilterProxyModel;
    }

    /**
     * @brief Toggle column visibility. Updates `Column::visible` and the
     * header. No-op for an out-of-range index. Public for tests and
     * the View menu.
     * @param logicalIndex The logical column index.
     * @param visible The new visibility state.
     */
    void SetColumnVisible(int logicalIndex, bool visible);

    /**
     * @brief Open the per-column editor dialog modally on @p columnIndex.
     * Reached from the header right-click menu, the diagnostics
     * dialog (row double-click), and the columns manager's
     * Edit\u2026 button. No-op for out-of-range indices.
     * @param columnIndex The logical column index.
     */
    void EditColumn(int columnIndex);

    /**
     * @brief Show the modeless `ColumnsManagerDialog` (constructed lazily).
     * A second call raises the existing instance.
     */
    void ShowColumnsManager();

    /**
     * @brief Show + raise the Record Details dock and pin it to @p proxyIndex
     * (mapped to a source row internally). Invalid index: no-op.
     * @param proxyIndex The `proxyIndex` value.
     */
    void ShowRecordDetailsForProxyIndex(const QModelIndex &proxyIndex);

    /**
     * @brief Sync the dock to the table's current selection. No-op when the
     * dock is hidden (avoids work on an invisible widget).
     */
    void UpdateRecordDetailsFromSelection();

    /**
     * @brief Open a standalone `RecordDetailWindow` snapshot of source row
     * @p sourceRow. Out-of-range rows are a no-op.
     * @param sourceRow The source-model row index.
     */
    void OpenRecordDetailWindow(int sourceRow);

    /**
     * @brief Push every `Column::visible` flag to the header. Idempotent;
     * run after a load or reorder.
     */
    void ApplyColumnVisibility();

    /**
     * @brief Install (or detach) the icon-pill delegate on the current
     * first `Type::Level` column. Idempotent; detaches from the
     * previous column when the level column has moved, and
     * detaches entirely when no level column exists. Safe on the
     * streaming hot path (at most two `setItemDelegateForColumn`
     * calls).
     *
     * View column index == source column index here because both
     * proxies (`LogFilterModel`, `RowOrderProxyModel`) pass
     * `columnCount` through 1:1. A column-reordering proxy
     * would have to remap.
     */
    void ApplyLevelCellDelegate();

    /**
     * @brief Restore the header so visual == logical for every section.
     * Suppresses re-entry into `OnHeaderSectionMoved` while doing
     * so. Idempotent.
     */
    void ResetHeaderToIdentity();

    /**
     * @brief Result of `BuildHeaderContextMenu`. Caller owns `menu`.
     */
    struct HeaderContextMenu
    {
        QMenu *menu = nullptr;
    };

    /**
     * @brief Build the right-click header menu for @p logicalColumn.
     * Caller owns `result.menu`. `result.menu` is null when
     * @p logicalColumn is out of range.
     * @param logicalColumn The logical column index.
     * @param parent The optional Qt parent.
     * @return The result described above.
     */
    [[nodiscard]] HeaderContextMenu BuildHeaderContextMenu(int logicalColumn, QWidget *parent = nullptr);

    /**
     * @brief Build the row right-click menu for source-model row @p sourceRow.
     * Always includes the "Anchor" sub-menu; adds "Show only newer/
     * older logs" actions when the row has a non-`monostate` timestamp
     * in the first `Type::Time` column.
     *
     * Returns null when the model is empty or @p sourceRow is out of
     * range. Caller owns the result; parented to @p parent (or
     * `mTableView` if null).
     * @param sourceRow The source-model row index.
     * @param parent The optional Qt parent.
     * @return The result described above.
     */
    [[nodiscard]] QMenu *BuildRowContextMenu(int sourceRow, QWidget *parent = nullptr);

    /**
     * @brief Returns the active session's simple filter leaves keyed by UUID.
     * @return A reference owned by the active session and invalidated with that session.
     */
    [[nodiscard]] const std::unordered_map<std::string, loglib::LeafRule> &Filters() const;

    /**
     * @brief Owned `LogModel`; non-null after construction.
     * @return The result described above.
     */
    [[nodiscard]] LogModel *Model() const
    {
        return mModel;
    }

    /**
     * @brief Source-model row indices that `File -> Export Filtered
     * Rows...` would walk, in the same top-to-bottom order the
     * user sees. Respects every proxy layer (`LogFilterModel`
     * sort, `RowOrderProxyModel` newest-first flip); when
     * @p selectionOnly is true, restricts to the current
     * selection. Public so tests can pin ordering without
     * going through the modal `ExportDialog`; production code
     * should use `ExportFilteredRows` instead.
     * @param selectionOnly The `selectionOnly` value.
     * @return The result described above.
     */
    [[nodiscard]] std::vector<int> CollectExportSourceRows(bool selectionOnly) const;

    /**
     * @brief Owned `AnchorManager`; non-null after construction.
     * @return The result described above.
     */
    [[nodiscard]] AnchorManager *Anchors() const noexcept
    {
        return mAnchors;
    }

    /**
     * @brief Owned `HighlightRuleSet`; non-null after construction.
     * Exposed for test inspection of the rebind signal path.
     * @return The result described above.
     */
    [[nodiscard]] HighlightRuleSet *Highlights() const noexcept
    {
        return mHighlights;
    }

    /**
     * @brief Select the next (forward=true) or previous anchored row in
     * visible (proxy) order, honouring sort + filter + newest-first
     * orientation. Wraps at the visible bounds. Surfaces a status-bar
     * note when no anchored row is visible. Wired to F2 / Shift+F2.
     * @param forward The `forward` value.
     */
    void JumpToAnchor(bool forward);

    /**
     * @brief Open a one-line note editor for the anchor at @p key. No-op
     * (with a status-bar hint) when the anchor is gone -- e.g. a
     * queued eviction or `Ctrl+0` between menu build and click.
     * Committed text is sanitised and forwarded to
     * `AnchorManager::SetAnchorNote`.
     *
     * Row context menu uses this path so it captures the anchor
     * *identity* (not a row index), preventing a mid-menu FIFO
     * eviction from redirecting the edit to a different row.
     * @param key The `key` value.
     */
    void EditAnchorNoteForKey(const AnchorManager::Key &key);

    /**
     * @brief Row-index wrapper around `EditAnchorNoteForKey`. Used by F4
     * and tests; the row-menu path captures a key directly to
     * avoid the eviction race.
     * @param sourceRow The source-model row index.
     */
    void EditAnchorNoteForRow(int sourceRow);

    /**
     * @brief F4 handler: resolves the focused proxy row to a source row
     * and calls `EditAnchorNoteForRow`. Shows a status-bar hint
     * when no row is focused or the row isn't anchored.
     */
    void EditAnchorNoteOnCurrentRow();

    /**
     * @brief Scroll to source row @p sourceRow and make it the sole
     * selection. No-op on a negative row, unready model, or a row
     * that is currently filtered out (the latter shows a status bar
     * note). Used by the Anchors dock for jump targets.
     * @param sourceRow The source-model row index.
     */
    void SelectSourceRow(int sourceRow);

    /**
     * @brief Pop the "Go to Line..." modal (`Ctrl+G`). Line numbers are
     * 1-based over the source model as it currently is; line 1 is
     * always the earliest retained row (streaming FIFO eviction
     * may have dropped older rows, so numbers need not match the
     * source file's numbering). Newest-first display reversal only
     * affects rendering, not the number the user types. Rejects
     * and status-bar-hints on any error; a valid row hands off to
     * `SelectSourceRow`. Post-dialog work lives in
     * `ExecuteGotoLine` so tests can drive it without a modal.
     */
    void GotoLine();

    /**
     * @brief Pop the "Go to Timestamp..." modal (`Ctrl+Shift+G`). Accepts
     * the current time column's `parseFormats`, two ISO fallbacks
     * (`%FT%T`, `%F %T`), and the relative shortcuts `-Nh` /
     * `-Nm`. Naive inputs (no `%z` / `%Z` in the winning format)
     * are shifted from the table's display time zone
     * (`loglib::CurrentZone()`) to UTC before the search. Lands
     * on the first matching row via `FindFirstRowAtOrAfter` +
     * `SelectSourceRow`, or status-bar-hints if none qualifies.
     */
    void GotoTimestamp();

    /**
     * @brief Result of `ParseGotoTimestampInput`. `micros` is epoch
     * microseconds; `isNaive` is true when the winning format had
     * no zone specifier (`%z` / `%Ez` / `%Z`) and the caller must
     * therefore shift `micros` through the display TZ before
     * comparing against stored (UTC-normalised) timestamps. The
     * relative-shortcut path always returns `isNaive == false`
     * since it derives from `system_clock::now()` (already UTC).
     */
    struct GotoTimestampParse
    {
        int64_t micros = 0;
        bool isNaive = false;
    };

    /**
     * @brief Pure parser for `GotoTimestamp`. Tries, in order: the
     * relative shortcut `^[+-]?\s*(\d+)\s*([hm])\s*$` (case-
     * insensitive; `+` / no-sign also mean "N units ago", matching
     * lnav / less); each entry of @p columnParseFormats via
     * `loglib::TryParseTimestamp`; the ISO fallbacks `%FT%T` and
     * `%F %T`. Returns `std::nullopt` on no match or on relative-
     * shortcut overflow (silent wrap would jump the user forward,
     * which is worse than a hint). Static so unit tests can drive
     * it without a `MainWindow` instance.
     * @param input The input value.
     * @param columnParseFormats The `columnParseFormats` value.
     * @param now The `now` value.
     * @return The result described above.
     */
    [[nodiscard]] static std::optional<GotoTimestampParse> ParseGotoTimestampInput(
        const QString &input,
        const std::vector<std::string> &columnParseFormats,
        std::chrono::system_clock::time_point now
    );

    /**
     * @brief First row whose timestamp on @p timeCol is `>= targetMicros`
     * AND is currently visible through the outer proxy (respects
     * the active row filter). Returns the source-model row index
     * or `-1` when nothing qualifies.
     *
     * Three branches, picked from (user-sort, monotonicity):
     *
     * * **Fast path** -- no user sort, `TimestampsAreMonotonic()`
     *   true. Binary-search source rows (O(log N)); on a filter
     *   miss, walk the outer proxy for the smallest visible row
     *   with a valid ts `>= lo` (O(N_visible) worst case, not
     *   O(N_source)). Missing timestamps are skipped, not
     *   `-inf`-treated.
     * * **Non-monotonic path** -- no user sort, monotonicity
     *   false (multi-file `Append`, rotation, clock skew). Walk
     *   the outer proxy and pick the visible row with the
     *   smallest ts satisfying `>= target` (chronologically
     *   earliest match).
     * * **User-sort path** -- header sort active. Linear scan in
     *   display order and return the first proxy row whose source
     *   ts qualifies, so "first" honours the user's sort.
     *
     * Public so tests can drive all three branches directly.
     * @param timeCol The `timeCol` value.
     * @param targetMicros The `targetMicros` value.
     * @return The result described above.
     */
    [[nodiscard]] int FindFirstRowAtOrAfter(int timeCol, int64_t targetMicros) const;

    /**
     * @brief Jump the table to the first row in histogram bucket
     * @p bucketIndex. Wired to `HistogramDock::bucketClicked`.
     * @param bucketIndex The `bucketIndex` value.
     */
    void JumpToFirstRowInBucket(std::size_t bucketIndex);

    /**
     * @brief Scroll the table so proxy row @p proxyRow is centred.
     * Wired to `OverviewRailWidget::proxyRowClicked`; no-op when
     * the row is out of range.
     *
     * @p replaceSelection: `true` on a fresh rail click (clears
     * the existing selection and selects just @p proxyRow);
     * `false` during a drag scrub (leaves selection alone so a
     * multi-row selection survives a rail scroll).
     *
     * Always disengages Follow newest: rail navigation is
     * intentional browsing, and `scrollTo` alone wouldn't fire
     * `userScrolledAwayFromTail`.
     * @param proxyRow The proxy-model row index.
     * @param replaceSelection The `replaceSelection` value.
     */
    void ScrollToProxyRow(int proxyRow, bool replaceSelection = true);

    /**
     * @brief Attach / detach `mOverviewRailWidget` on the table view,
     * persist visibility to `QSettings("ui/showOverviewRail")`,
     * and mirror the state onto `mActionToggleOverviewRail`.
     * Idempotent so the load-time seed and the user toggle share
     * one code path.
     * @param visible The new visibility state.
     */
    void SetOverviewRailVisible(bool visible);

    /**
     * @brief Install a `Type::Time` filter on
     * `[fromEpochMicros, toEpochMicros]` for the histogram's time
     * column. Wired to `HistogramDock::timeRangeSelected`; no-op
     * when the log has no time column.
     * @param fromEpochMicros The `fromEpochMicros` value.
     * @param toEpochMicros The `toEpochMicros` value.
     */
    void AddTimeRangeFilterFromHistogram(qint64 fromEpochMicros, qint64 toEpochMicros);

#ifdef LOGAPP_BUILD_TESTING
    /**
     * @brief Test-only session-mode override so display-order tests can
     * exercise the `Static` branch without a real open flow.
     */
    enum class TestSessionMode
    {
        Idle,
        Static,
        LiveTail,
    };
    void SetSessionModeForTest(TestSessionMode mode);

    /**
     * @brief Test-only reader for the current session mode.
     * @return The result described above.
     */
    [[nodiscard]] TestSessionMode SessionModeForTest() const noexcept;

    /**
     * @brief Test-only entry to the `TryLoadAsConfiguration` path
     * (production gates it behind `QFileDialog`).
     * @param file The file path to use.
     * @return The result described above.
     */
    bool TryLoadAsConfigurationForTest(const QString &file);

    /**
     * @brief Test-only entry to `SetConfigurationUiEnabled` so the
     * column-management gate can be exercised without a real
     * streaming session.
     * @param enabled The new enabled state.
     */
    void SetConfigurationUiEnabledForTest(bool enabled);

    /**
     * @brief Test-only entries to `SaveConfiguration` / `LoadConfiguration`
     * that bypass the file dialog. `scope` defaults to `Full` so
     * existing tests (written against the old single-action save)
     * keep passing; pass `SaveScope::ColumnsOnly` for the
     * "Save Configuration\u2026" path.
     * @param path The path to use.
     * @param scope The persistence scope.
     */
    void SaveConfigurationToPathForTest(const QString &path, loglib::SaveScope scope = loglib::SaveScope::Full);
    void LoadConfigurationFromPathForTest(const QString &path);

    /**
     * @brief When true, `ShowDroppedFiltersDialog` skips the modal and
     * only updates the test counter (modals block any headless
     * QtTest thread). Default false.
     * @param suppress The `suppress` value.
     */
    void SetSuppressDialogsForTest(bool suppress);

    /**
     * @brief Answer consumed by the next close-decision prompt in tests.
     */
    enum class ClosePromptChoiceForTest : std::uint8_t
    {
        Discard,
        Cancel,
    };

    /**
     * @brief Queues the next close-prompt answer for tests.
     *
     * Each prompt in `PrepareSessionClose` consumes one queued choice.
     * When the queue is empty, suppressed dialogs still treat a prompt
     * as Discard.
     *
     * @param choice The answer to consume for the next prompt.
     */
    void QueueClosePromptChoiceForTest(ClosePromptChoiceForTest choice)
    {
        mClosePromptChoicesForTest.push_back(choice);
    }

    /**
     * @brief Forces the next close-decision autosave attempt to fail.
     *
     * Consumed once by `AutoSaveSessionSnapshot` after the helper
     * decides a snapshot should be written.
     *
     * @param fail Whether the next write should fail.
     */
    void SetFailNextAutoSaveForTest(bool fail = true)
    {
        mFailNextAutoSaveForTest = fail;
    }

    /**
     * @brief Returns whether modal dialogs are suppressed for tests.
     * @return The current suppression state.
     */
    [[nodiscard]] bool SuppressDialogsForTest() const noexcept
    {
        return mSuppressDialogsForTest;
    }

    /**
     * @brief Filters dropped on the most recent
     * `LoadConfigurationFromPathForTest` call. Reset on each load.
     * @return The result described above.
     */
    [[nodiscard]] int LastDroppedFilterCountForTest() const;

    /**
     * @brief Installs a simple-mode filter on the active session, including the Filters menu.
     * @param filterId Filter identity stored on the menu action.
     * @param filter Filter rule to install.
     */
    void AddLogFilterForTest(const QString &filterId, const loglib::LeafRule &filter)
    {
        AddLogFilter(filterId, filter);
    }

    /**
     * @brief Test-only setter for `mCurrentSource`. Lets fixture-driven
     * tests assert the descriptor round-trips through Save Session
     * without running a real open path.
     * @param source The `source` value.
     */
    void SetCurrentSourceForTest(std::optional<loglib::LogConfiguration::Source> source);

    /**
     * @brief Test-only read accessor for `mCurrentSource`; lets tests
     * inspect the descriptor after an open or load.
     * @return The result described above.
     */
    [[nodiscard]] const std::optional<loglib::LogConfiguration::Source> &CurrentSourceForTest() const noexcept;

    /**
     * @brief Test-only accessor for the source label used by
     * `UpdateStreamingStatus`.
     * @return The result described above.
     */
    [[nodiscard]] const QString &StreamingFileNameForTest() const noexcept
    {
        return mSession->StreamingFileName();
    }

    /**
     * @brief Test-only entry to `ShowRowContextMenu` so tests can pin
     * right-click selection-adoption rules without a real mouse
     * event. Callers should close any popup that opens.
     * @param pos The position in widget coordinates.
     */
    void ShowRowContextMenuForTest(const QPoint &pos)
    {
        ShowRowContextMenu(pos);
    }

    /**
     * @brief Test-only entry to the queued static-files open path,
     * bypassing the file dialog and modifier sniff.
     * @param files The file paths to process.
     * @param mode The operation mode.
     */
    void OpenFilesForTest(const QStringList &files, OpenMode mode);

    /**
     * @brief Test-only entry to the mixed-input dispatcher. Returns the
     * branch the dispatcher took so tests can assert on the shape
     * without scraping the status bar.
     * @param files The file paths to process.
     * @param logMode The `logMode` value.
     * @return The result described above.
     */
    MixedInputDispatch OpenMixedFilesForTest(const QStringList &files, OpenMode logMode);

    /**
     * @brief Drive the post-dialog body of `OpenLogStream` with @p filePath.
     * Lets tests exercise the live-tail open path without a real
     * modal `QFileDialog`.
     * @param filePath The `filePath` value.
     */
    void OpenLogStreamForTest(const QString &filePath);

    /**
     * @brief Test-only check for a pending historical-prefix promotion.
     * @return The result described above.
     */
    [[nodiscard]] bool HasPendingLiveTailForTest() const noexcept
    {
        return mSession->HasPendingLiveTailPromotion();
    }

    /**
     * @brief Test-only setter for pending live-tail promotion state.
     * @param primary The `primary` value.
     * @param retention The `retention` value.
     */
    void SeedPendingLiveTailForTest(const QString &primary, size_t retention)
    {
        mSession->SetPendingLiveTailPromotion(primary, retention);
    }

    /**
     * @brief Test-only entry to the no-prefix live-tail rescue path.
     * @param primary The `primary` value.
     * @param retention The `retention` value.
     */
    void TriggerRescueLiveTailForTest(const QString &primary, size_t retention);

    /**
     * @brief Test-only entry to `UndoRotationExpansion`.
     */
    void UndoRotationExpansionForTest()
    {
        UndoRotationExpansion();
    }

    /**
     * @brief Original inputs captured for the most recent expansion.
     * @return The result described above.
     */
    [[nodiscard]] const QStringList &LastRotationExpansionOriginalInputsForTest() const noexcept
    {
        return mSession->LastRotationExpansionOriginalInputs();
    }

    /**
     * @brief Seed expansion-undo state. Live-tail state reopens through
     * `OpenLogStreamFromPath`; other state uses the static queue.
     * @param originalInputs The `originalInputs` value.
     * @param wasLiveTail The `wasLiveTail` value.
     */
    void SeedLastRotationExpansionForTest(const QStringList &originalInputs, bool wasLiveTail)
    {
        mSession->SetLastRotationExpansion(originalInputs, wasLiveTail);
        if (mActionUndoRotationExpansion != nullptr)
        {
            mActionUndoRotationExpansion->setEnabled(!originalInputs.isEmpty());
        }
    }

    /**
     * @brief Test-only reader for the expansion-undo action state.
     * @return The result described above.
     */
    [[nodiscard]] bool IsUndoRotationExpansionEnabledForTest() const noexcept
    {
        return mActionUndoRotationExpansion != nullptr && mActionUndoRotationExpansion->isEnabled();
    }

    /**
     * @brief Test seam; forwards a producer and pre-read bytes to
     * `OpenStdinStreamFromProducer`.
     * @param producer The `producer` value.
     * @param peek The `peek` value.
     */
    void OpenStdinStreamForTest(std::unique_ptr<loglib::BytesProducer> producer, std::string peek);

    /**
     * @brief Starts a new session while suppressing confirmation dialogs.
     *
     * The previous suppression state is restored before returning.
     */
    void NewSessionForTest()
    {
        const bool previous = SuppressDialogsForTest();
        SetSuppressDialogsForTest(true);
        NewSession();
        SetSuppressDialogsForTest(previous);
    }

    /**
     * @brief Forwards `NewSession` without changing dialog suppression.
     *
     * Use this when a queued close-prompt choice or a forced autosave
     * failure should drive the close decision.
     */
    void InvokeNewSessionForTest()
    {
        NewSession();
    }

    /**
     * @brief Test-only forwarder to the `OpenRecentSession` private slot.
     * @param uuid The `uuid` value.
     */
    void OpenRecentSessionForTest(const QString &uuid)
    {
        OpenRecentSession(uuid);
    }

    /**
     * @brief Test seam for the `JumpToNewestRow` private helper; lets
     * the filtered-newest-row fallback be exercised without
     * synthesising a real pill click.
     */
    void JumpToNewestRowForTest()
    {
        JumpToNewestRow();
    }

    /**
     * @brief Test-only accessor for the in-flight decompression flag. Lets
     * timing-sensitive tests confirm a worker is armed without
     * racing on wall-clock sleeps.
     * @return The result described above.
     */
    [[nodiscard]] bool IsDecompressionInFlightForTest() const noexcept
    {
        return mSession->IsDecompressionInFlight();
    }
    /**
     * @brief Return whether a pending bundle may apply embedded configuration.
     * @return The result described above.
     */
    [[nodiscard]] bool AppliesEmbeddedBundleConfigForNextOpenForTest() const noexcept
    {
        return mSession->ShouldApplyEmbeddedBundleConfig();
    }
    /**
     * @brief Simulate superseding a pending bundle decompression.
     */
    void SimulateSupersededBundleDecompressionForTest()
    {
        mSession->SetApplyEmbeddedBundleConfigForPath(QStringLiteral("simulated-bundle.slvbundle"));
        mSession->SetDecompressionInFlight(true);
        CancelInFlightDecompression();
    }

    /**
     * @brief Test-only cancel injection: raises the same stop request
     * `QProgressDialog::canceled` sends. Needed because the dialog
     * is suppressed under `SetSuppressDialogsForTest`, making the
     * production cancel wiring unreachable from tests. Callers must
     * pump the event loop to drive the finished slot. No-op when
     * no decompression is in flight.
     */
    void RequestDecompressionCancelForTest()
    {
        if (mSession->IsDecompressionInFlight())
        {
            mSession->MutableDecompressionStopSource().request_stop();
        }
    }

    /**
     * @brief Export a bundle without showing the dialog, using production
     * preflight checks and default zstd settings.
     * @param destination The `destination` value.
     */
    void ExportSessionBundleToPathForTest(const QString &destination);

    /**
     * @brief Return whether an export worker is active.
     * @return The result described above.
     */
    [[nodiscard]] bool IsExportInFlightForTest() const noexcept
    {
        return mSession->IsExportInFlight();
    }

    /**
     * @brief Test seam replaying the anchor-note commit path without a
     * modal `QInputDialog`. Applies the same row-must-be-anchored
     * guard, then forwards @p note to `SetAnchorNote` (which
     * sanitises before storage). Returns true iff the row was
     * anchored (identical-note no-ops still return true). Returns
     * false on an unanchored row -- no ghost anchor is spawned.
     * @param sourceRow The source-model row index.
     * @param note The `note` value.
     * @return The result described above.
     */
    bool SubmitAnchorNoteForRowForTest(int sourceRow, const QString &note);

    /**
     * @brief Test seam replaying the post-`exec` body of `GotoLine`
     * without a modal dialog. Lets tests drive the range check
     * (shrink-while-modal-open simulation: pass a line larger
     * than the current row count) and the filter-visibility hint.
     * @param input The input value.
     */
    void ExecuteGotoLineForTest(const QString &input);

    /**
     * @brief Test seam replaying the post-`exec` body of `GotoTimestamp`
     * without a modal dialog. @p now pins the clock so relative
     * shortcuts (`-1h`) are deterministic. Covers the sticky-
     * input update, error hints, and `FindFirstRowAtOrAfter`
     * handoff -- none reachable through `ParseGotoTimestampInput`.
     * @param input The input value.
     * @param now The `now` value.
     */
    void ExecuteGotoTimestampForTest(const QString &input, std::chrono::system_clock::time_point now);

    /**
     * @brief Test-only accessor for the sticky Goto Timestamp input so
     * tests can pin the session-boundary clear.
     * @return The result described above.
     */
    [[nodiscard]] QString LastGotoTimestampInputForTest() const;

    /**
     * @brief Test seam: force `LogModel::TimestampsAreMonotonic()` false
     * so `FindFirstRowAtOrAfter` takes its non-monotonic branch
     * without a real inversion. Irreversible.
     */
    void ForceTimestampsNonMonotonicForTest();

    /**
     * @brief Test seam replaying `OpenAdvancedFilter`'s post-`exec` body
     * without a modal dialog. Runs the full leaf-extraction +
     * mirror + recompile pipeline so tests can pin the simple/
     * advanced split behaviour end-to-end.
     * @param expression The `expression` value.
     */
    void CommitAdvancedFilterForTest(loglib::FilterExpression expression)
    {
        ApplyAdvancedFilterResult(std::move(expression));
    }
#endif

protected:
    bool event(QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

    /**
     * @brief Re-tint every themed icon on a palette / style / theme /
     * DPR change so a Light <-> Dark flip (or a monitor move
     * between different scale factors) keeps the Lucide glyphs
     * aligned with the new `QPalette::WindowText` and rasterised
     * at the new device-pixel ratio. The companion hook in
     * `OnThemeChanged` covers application-driven theme switches;
     * this hook catches OS-level changes that bypass
     * `ThemeControl` (Windows light/dark notification arrives as
     * `QEvent::ThemeChange` and may not always be preceded by a
     * palette diff). Same idiom as `FindRecordWidget::changeEvent`.
     * @param event The event to handle.
     */
    void changeEvent(QEvent *event) override;

private slots:
    /**
     * @brief Discard the current session and return to an empty view.
     * Bound to `actionNewSession` (Ctrl+N).
     */
    void NewSession();
    /**
     * @brief Spawn a new top-level `MainWindow` sharing this manager.
     * Heap-allocated with `Qt::WA_DeleteOnClose`. No-op in
     * no-history mode.
     */
    void NewWindow();
    /**
     * @brief Rebuild the `File -> Recent Sessions` submenu from the
     * manager's live list. Connected to `aboutToShow` so we never
     * paint stale entries.
     */
    void RebuildRecentSessionsMenu();
    /**
     * @brief Reopen the recents entry @p uuid. Pre-flights the parse,
     * then `NewSession` + `DoLoadConfiguration` to restore columns
     * / filters / sort / source. Locators are streamed in `Append`
     * mode (non-destructive on the now-empty model). On success
     * `mAutoSaveUuid` is pinned to @p uuid so further edits update
     * that recents entry instead of forking a new one.
     * @param uuid The `uuid` value.
     */
    void OpenRecentSession(const QString &uuid);

    /**
     * @brief Shared tail of `RestoreLastSessionFromPath` and
     * `OpenRecentSession`: stream `mCurrentSource->locators` or
     * short-circuit on unsupported / empty sources.
     * @p informIfNonFile picks between a silent skip (restore-on-
     * launch, never pop a dialog on startup) and a
     * `QMessageBox::information` (user-initiated click).
     * @param informIfNonFile The `informIfNonFile` value.
     */
    void StreamFromCurrentSourceOrSkip(bool informIfNonFile);
    void OpenFiles();
    void OpenLogStream();
    /**
     * @brief Pop the `NetworkStreamDialog`, build the matching producer, and
     * call `LogModel::BeginStreaming`.
     */
    void OpenNetworkStream();
    /**
     * @brief "Save Configuration\u2026" -- writes the portable
     * columns-only slice.
     */
    void SaveConfiguration();
    /**
     * @brief "Save Session\u2026" -- writes columns + filters + sort + source.
     * @return `true` when a session file was written.
     */
    bool SaveSession();
    /**
     * @brief Loads either shape; missing session fields default to inert
     * values.
     */
    void LoadConfiguration();

    /**
     * @brief "Export Filtered Rows\u2026" -- pops the export dialog and
     * dispatches an async worker that writes the current filter
     * slice in one of the four supported formats (`JSON Lines`,
     * `CSV`, `Source snapshot`, `Markdown table`). Progress and
     * cancel run through a non-modal per-window `QProgressDialog`;
     * user cancel unwinds via `slv::exports::ExportCancelled`.
     */
    void ExportFilteredRows();

    /**
     * @brief Export all retained rows and view state to `.slvbundle`.
     * Shares asynchronous export state with filtered-row export.
     */
    void ExportSessionBundle();

    /**
     * @brief Show the `ConfigurationDiagnosticsDialog` (constructed lazily).
     * A second call raises the existing instance.
     */
    void ShowConfigurationDiagnostics();

    /**
     * @brief Refresh the status-bar mismatch summary. Wired to
     * `LogModel::columnHealthChanged`; hides the button when zero
     * mismatches are present.
     */
    void UpdateDiagnosticsStatus();

    /**
     * @brief Refresh the status-bar parse-errors indicator. Hooked to
     * `ParseErrorsDock::countChanged`; hides when the dock is empty.
     * @param count The `count` value.
     * @param droppedCount The `droppedCount` value.
     */
    void UpdateParseErrorsStatus(int count, int droppedCount);

    /**
     * @brief Refresh the "*n* shown of *m*" status-bar label and toggle
     * the inline Clear-filters button. Wired to source + proxy
     * row signals; the label tracks `mModel->rowCount()` (not
     * `IsSessionActive`) so the indicator survives the post-load
     * `Static -> Idle` flip and stays visible while the user
     * browses the parsed rows. Hides both widgets when the source
     * model is empty.
     */
    void UpdateRowsShownStatus();

    /**
     * @brief Recount matches for the current find query and push the
     * result back into the find bar. Caches the row list keyed by
     * `(needle, wildcards, regex)` so Next / Previous clicks reuse
     * the scan and just resolve the new `i` via binary search.
     * Skipped when the bar is hidden / proxy is unset / needle empty.
     * @param text The `text` value.
     * @param wildcards The `wildcards` value.
     * @param regularExpressions The `regularExpressions` value.
     */
    void UpdateFindMatchCount(const QString &text, bool wildcards, bool regularExpressions);

    /**
     * @brief Drop the cached match-row list. Wired to model resets and
     * proxy layout changes so a stale cache cannot survive.
     */
    void InvalidateFindMatchCache();

    /**
     * @brief Projects the active find cache onto the overview rail.
     *
     * Hidden find UI is ignored. Missing or mismatched bucket counts trigger a
     * recount so capped row samples cannot bias the rail.
     */
    void PushFindMatchesToOverviewRail();

    /**
     * @brief Centralised invalidate + debounced re-request. Wired to every
     * model / proxy signal that can change the match set; a sync
     * re-scan per signal would melt under streaming.
     */
    void OnFindCacheInvalidated();

    /**
     * @brief Updates filter state after an enum column changes.
     * @param reason The kind of enum-column transition.
     * @param columnIndex The affected logical column index.
     */
    void OnEnumColumnsChangedApplyFilterRebuild(EnumColumnsChangeReason reason, int columnIndex);

    /**
     * @brief Applies shared table policies and delegates to a session view.
     * @param view The view to configure; `nullptr` is ignored.
     */
    void ApplyTableChromeToView(LogSessionView *view);

    /**
     * @brief Rebuilds the Filters menu from the active session without changing filters.
     */
    void RebuildFilterMenuFromActiveSession();

    /**
     * @brief Applies and clears UI work queued while this session was backgrounded.
     * @param session The session that just became selected; `nullptr` is ignored.
     */
    void ApplyPendingPresentation(LogSession *session);

    void Find();
    void FindRecords(const QString &text, bool next, bool wildcards, bool regularExpressions);

    /**
     * @brief Add a filter rule, optionally opening the editor. Pass
     * `openEditor = false` on the config-load path so a restored
     * filter does not pop the editor. When @p filter is empty and
     * @p initialColumn >= 0, the editor preselects that column
     * (used by the header "Add filter on ..." entry). Ignored
     * when @p filter has a value (it already pins the row).
     * @param filterId The `filterId` value.
     * @param filter The `filter` value.
     * @param openEditor The `openEditor` value.
     * @param initialColumn The `initialColumn` value.
     */
    void AddFilter(
        const QString &filterId,
        const std::optional<loglib::LeafRule> &filter = std::nullopt,
        bool openEditor = true,
        int initialColumn = -1
    );
    void ClearAllFilters();
    /**
     * @brief Open the modal Advanced Filter editor seeded with the
     * current `LogConfiguration::expression`. On accept, dispatches
     * to `ApplyAdvancedFilterResult`.
     */
    void OpenAdvancedFilter();

    /**
     * @brief Post-dialog body of `OpenAdvancedFilter`. Extracts top-level
     * Leaves back into `mSimpleLeaves` (mirroring load) so a mixed
     * tree like `svc:x AND NOT lvl:info` still shows a Filters-menu
     * entry per representable leaf; the non-Leaf remainder stays
     * on the expression. Exposed via `CommitAdvancedFilterForTest`.
     * @param result The operation result.
     */
    void ApplyAdvancedFilterResult(loglib::FilterExpression result);
    /**
     * @brief Drop the active column sort via
     * `mTableView->sortByColumn(-1, ...)` so proxy, header, and
     * persisted config stay in lockstep. Bound to
     * `actionClearSort` (Sort menu, toolbar, status bar,
     * header right-click).
     */
    void ClearSort();
    /**
     * @brief Remove a single filter rule. Pass `deferSync = true` when the
     * caller (e.g. a submit slot) immediately re-adds the filter
     * so the mirror + rule rebuild only run once.
     * @param filterID The `filterID` value.
     * @param deferSync The `deferSync` value.
     */
    void ClearFilter(const QString &filterID, bool deferSync = false);
    void FilterSubmitted(const QString &filterID, int row, const QString &filterString, int matchType);
    /**
     * @brief Slot for `FilterEditor::FilterTimeStampSubmitted`. `std::nullopt`
     * on a bound leaves that side unbounded (the predicate substitutes
     * INT64 sentinels at construction); both-nullopt is rejected.
     * @param filterID The `filterID` value.
     * @param row The row index.
     * @param beginTimeStamp The `beginTimeStamp` value.
     * @param endTimeStamp The `endTimeStamp` value.
     */
    void FilterTimeStampSubmitted(
        const QString &filterID, int row, std::optional<qint64> beginTimeStamp, std::optional<qint64> endTimeStamp
    );
    void FilterEnumSubmitted(const QString &filterID, int row, const QStringList &selectedValues);
    /**
     * @brief Slot for `FilterEditor::FilterNumericRangeSubmitted`. Either bound
     * may be `std::nullopt` to leave that side unbounded.
     * @param filterID The `filterID` value.
     * @param row The row index.
     * @param minValue The `minValue` value.
     * @param maxValue The `maxValue` value.
     */
    void FilterNumericRangeSubmitted(
        const QString &filterID, int row, std::optional<double> minValue, std::optional<double> maxValue
    );
    /**
     * @brief Slot for `FilterEditor::FilterBooleanSubmitted`.
     * @param filterID The `filterID` value.
     * @param row The row index.
     * @param includeTrue The `includeTrue` value.
     * @param includeFalse The `includeFalse` value.
     */
    void FilterBooleanSubmitted(const QString &filterID, int row, bool includeTrue, bool includeFalse);

    /**
     * @brief Pause / resume on the bridging sink. Bound to `actionPauseStream`.
     * @param paused The `paused` value.
     */
    void TogglePauseStream(bool paused);

    /**
     * @brief Stop the active stream. Bound to `actionStopStream`.
     */
    void StopStream();

    /**
     * @brief Rotation event re-emitted on the GUI thread; flashes the
     * `— rotated` status-bar suffix.
     */
    void OnRotationDetected();

    /**
     * @brief Producer status transition; latches `mSourceWaiting` and
     * refreshes the status bar.
     * @param status The `status` value.
     */
    void OnSourceStatusChanged(loglib::SourceStatus status);

    /**
     * @brief Translate a header drag into a source-side `LogModel::
     * MoveColumn`, then restore visual == logical. The runtime
     * filter remap and visibility re-apply happen in
     * `OnSourceColumnsMoved`, which also catches implicit moves
     * (e.g. mid-stream timestamp bubbling).
     * @param logicalIndex The logical column index.
     * @param oldVisualIndex The `oldVisualIndex` value.
     * @param newVisualIndex The `newVisualIndex` value.
     */
    void OnHeaderSectionMoved(int logicalIndex, int oldVisualIndex, int newVisualIndex);

    /**
     * @brief `LogModel::columnsMoved` slot: re-apply `Column::visible`
     * and rebuild the compiled filter (which caches resolved
     * column indices; leaves themselves bind by `columnKeys`).
     * Handles both header-drag and streaming-induced column moves
     * (the timestamp bubble in `LogModel::AppendBatch`). Qt
     * clears hidden flags via `initializeSections()` on a
     * zero-row source, hence the visibility re-apply.
     * @param parent The optional Qt parent.
     * @param first The `first` value.
     * @param last The `last` value.
     * @param destParent The `destParent` value.
     * @param destColumn The `destColumn` value.
     */
    void OnSourceColumnsMoved(
        const QModelIndex &parent, int first, int last, const QModelIndex &destParent, int destColumn
    );

    /**
     * @brief Build and show the header context menu at @p pos.
     * @param pos The position in widget coordinates.
     */
    void ShowHeaderContextMenu(const QPoint &pos);

    /**
     * @brief Build and show the row right-click menu at @p pos (viewport
     * coords). Adds an inclusive time-range filter pinned to the first
     * `Type::Time` column, boundary = clicked row's timestamp.
     * @param pos The position in widget coordinates.
     */
    void ShowRowContextMenu(const QPoint &pos);

    /**
     * @brief Rebuild the `View` menu on each `aboutToShow`. Each column
     * gets a checkable action that toggles `Column::visible`.
     * Always reachable, so it can restore visibility when every
     * header section is hidden.
     */
    void RebuildViewMenu();

private:
    /**
     * @brief Updates shell aliases for the active session and optional view.
     * @param session The non-null session whose models become active aliases.
     * @param view The view whose widgets become active aliases, or `nullptr` before view construction.
     *
     * This function does not alter scoped connections.
     */
    void SetActiveSessionAliases(LogSession *session, LogSessionView *view) noexcept;

    /**
     * @brief Installs all subscriptions scoped to the active session and view.
     *
     * Connections are retained in `mSessionConnections` for atomic removal on
     * tab switches and teardown. Window-scoped subscriptions are excluded.
     */
    void InstallActiveSessionConnections();

    // Tab lifecycle.

    /**
     * @brief Creates and appends an empty session tab.
     * @param makeActive Whether to activate the new tab.
     * @return The new session identifier, or an invalid identifier if the tab widget is unavailable.
     */
    SessionInstanceId AddNewTab(bool makeActive = true);

    /**
     * @brief Closes a tab and schedules its session and view for deletion.
     * @param index The tab index; invalid indices are ignored.
     *
     * Persistent connections are disconnected and the tab is unhosted before
     * workers are drained so queued completions cannot start the next file or
     * mix another session's model aliases with a surviving view. Sibling tabs
     * are left running. The final tab closes the window. A cancelled
     * dirty-session prompt leaves the tab and its workers unchanged.
     */
    void CloseTabAtIndex(int index);

    /**
     * @brief Rebinds session-scoped state after the active tab changes.
     * @param newIndex The newly active tab index.
     *
     * Invalid, suppressed, or same-session notifications are ignored.
     */
    void OnActiveTabChanged(int newIndex);

    /**
     * @brief Refreshes the label and tooltip for a session's tab.
     * @param session The hosted session; missing sessions are ignored.
     */
    void RefreshTabChrome(const LogSession *session);

    /** @brief Suppresses tab-change handling while tab records are inconsistent. */
    struct SuppressActiveTabChangeScope
    {
        /**
         * @brief Enables suppression for the scope lifetime.
         * @param owner The window whose suppression state is managed.
         */
        explicit SuppressActiveTabChangeScope(MainWindow &owner) noexcept
            : mOwner(owner), mPrevious(owner.mSuppressActiveTabChange)
        {
            mOwner.mSuppressActiveTabChange = true;
        }
        ~SuppressActiveTabChangeScope()
        {
            mOwner.mSuppressActiveTabChange = mPrevious;
        }
        SuppressActiveTabChangeScope(const SuppressActiveTabChangeScope &) = delete;
        SuppressActiveTabChangeScope &operator=(const SuppressActiveTabChangeScope &) = delete;
        SuppressActiveTabChangeScope(SuppressActiveTabChangeScope &&) = delete;
        SuppressActiveTabChangeScope &operator=(SuppressActiveTabChangeScope &&) = delete;

    private:
        MainWindow &mOwner;
        bool mPrevious;
    };

    /**
     * @brief Holds the active session's switch latch during destructive resets.
     *
     * The latch suppresses synchronous cancellation bookkeeping and is restored
     * on every exit path.
     */
    struct SessionSwitchScope
    {
        /**
         * @brief Enables the active session's switch latch.
         * @param owner The window whose active session is switching.
         */
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

    /**
     * @brief Temporarily routes completion handling to its originating session.
     *
     * Aliases and active-tab chrome are restored on destruction. While swapped,
     * shell-global UI writes are suppressed and parse errors remain origin-bound.
     */
    struct CompletionOriginSwapScope
    {
        /**
         * @brief Starts an origin-bound completion scope.
         * @param owner The window whose aliases may be rebound.
         * @param origin The session that started the operation.
         * @param originView The origin's view, or `nullptr` if it is unavailable.
         */
        CompletionOriginSwapScope(MainWindow &owner, LogSession *origin, LogSessionView *originView) noexcept;
        ~CompletionOriginSwapScope();
        CompletionOriginSwapScope(const CompletionOriginSwapScope &) = delete;
        CompletionOriginSwapScope &operator=(const CompletionOriginSwapScope &) = delete;
        CompletionOriginSwapScope(CompletionOriginSwapScope &&) = delete;
        CompletionOriginSwapScope &operator=(CompletionOriginSwapScope &&) = delete;

        /**
         * @brief Reports whether aliases were rebound.
         * @return `true` when the origin differed from the active session.
         */
        [[nodiscard]] bool didSwap() const noexcept
        {
            return mDidSwap;
        }

    private:
        MainWindow &mOwner;
        LogSession *mSavedSession;
        LogSessionView *mSavedView;
        bool mSavedBackgroundInFlight;
        bool mDidSwap;
    };

    /**
     * @brief Append the "Anchor" sub-menu (eight colour swatches +
     * "Remove anchor") to @p menu. Check state reflects the right-
     * clicked row's existing colour, but triggered actions operate
     * on the current selection (same as the `Ctrl+1..8` hotkeys).
     * No-op if model, theme, or anchor manager is missing.
     * @param menu The menu to populate.
     * @param sourceRow The source-model row index.
     */
    void AppendAnchorActionsToRowMenu(QMenu *menu, int sourceRow);

    /**
     * @brief Shared post-`exec` body of `GotoLine`. Re-checks the live
     * row count (catches a shrink while the modal was open),
     * probes filter visibility, then hands off to
     * `SelectSourceRow`. Emits a status hint on every rejection.
     * @param input The input value.
     */
    void ExecuteGotoLine(const QString &input);

    /**
     * @brief Shared post-`exec` body of `GotoTimestamp`. Updates the
     * sticky-input mirror, parses via `ParseGotoTimestampInput`,
     * shifts naive results through
     * `LocalMicrosecondsSinceEpochToUtc`, then hands off to
     * `FindFirstRowAtOrAfter` + `SelectSourceRow`. @p now is
     * pinned in tests, `system_clock::now()` in production.
     * @param input The input value.
     * @param now The `now` value.
     */
    void ExecuteGotoTimestamp(const QString &input, std::chrono::system_clock::time_point now);

    /**
     * @brief Logical index of the column whose `keys` match @p keys, or
     * `-1` if none. `keys` is the only identifier that survives a
     * reorder; menu lambdas use it to re-resolve the target column
     * at trigger time.
     * @param keys The `keys` value.
     * @return The result described above.
     */
    [[nodiscard]] int FindColumnIndexByKeys(const std::vector<std::string> &keys) const;

    /**
     * @brief Menu label for one column: the header, or `header [keys]`
     * when the header is shared with another column. Empty when
     * @p columnIndex is out of range. For all columns at once,
     * prefer `BuildAllColumnMenuLabels` (this entry point is O(N)
     * per call).
     * @param columnIndex The logical column index.
     * @return The result described above.
     */
    [[nodiscard]] QString ColumnMenuLabel(size_t columnIndex) const;

    /**
     * @brief Menu labels for every column in one O(N) pass (tallies
     * duplicate headers once and reuses the count). Use this from
     * the `View` menu rebuild instead of looping `ColumnMenuLabel`.
     * @return The result described above.
     */
    [[nodiscard]] std::vector<QString> BuildAllColumnMenuLabels() const;

    /**
     * @brief Try to load @p file as a `LogConfiguration`; returns true on
     * success.
     * @param file The file path to use.
     * @return The result described above.
     */
    bool TryLoadAsConfiguration(const QString &file);

    /**
     * @brief Funnel for drop / Open... / CLI inputs that may mix
     * configuration JSONs and log files. Each path is classified
     * via `FileLooksLikeConfiguration`:
     *
     * - Zero configs -> `StartStreamingOpenQueue(files, logMode)`.
     *   A sole `.slvbundle` also arms embedded-configuration apply
     *   on that session before the queue starts.
     * - One config, no logs -> `TryLoadAsConfiguration` (no reset).
     * - One config + N logs -> `DoLoadConfiguration` (full reset)
     *   then `StartStreamingOpenQueue(logs, Append)`.
     * - Two or more configs -> warning dialog, no state mutated.
     *
     * @p logMode is used only for the no-config branch (the mixed
     * branch is always `Append` since `DoLoadConfiguration` already
     * did the reset).
     *
     * The returned `appliedConfigPath` names the configuration the
     * dispatcher actually picked (not necessarily `files.front()`),
     * so callers can name it correctly in user-facing text.
     * @param files The file paths to process.
     * @param logMode The `logMode` value.
     * @return The result described above.
     */
    MixedInputResult DispatchMixedOpenInput(const QStringList &files, OpenMode logMode);

    /**
     * @brief Start a sequential streaming open of @p files.
     *
     * `OpenMode::Replace`: reset the model, clear runtime filters,
     * drop `mCurrentSource`, then queue the files (first via
     * `BeginStreaming`, subsequent via `AppendStreaming`).
     *
     * `OpenMode::Append`: keep the active static session intact and
     * queue @p files onto the back. With no active session it
     * behaves like `Replace` minus the destructive reset (so
     * previously-loaded columns / filters survive into the new
     * session). Live-tail / network sessions always force `Replace`.
     *
     * When @p applyEmbeddedBundlePath is non-empty, the session is
     * authorized to apply that bundle's embedded configuration. The
     * path is armed after any destructive reset and before the first
     * queued file starts, so `OnDecompressionFinished` still sees it
     * if the worker finishes during watcher `setFuture`.
     *
     * @param files The file paths to process.
     * @param mode The operation mode.
     * @param applyEmbeddedBundlePath Bundle path to arm, or empty.
     */
    void StartStreamingOpenQueue(
        QStringList files, OpenMode mode, const QString &applyEmbeddedBundlePath = QString()
    );

    /**
     * @brief Starts the next queued file or asynchronous decompression.
     *
     * Open and decompression failures accumulate in separate session-owned
     * buckets. Compressed inputs resume through `OnDecompressionFinished()`.
     */
    void StreamNextPendingFile();

    /**
     * @brief Dispatch an async `DecompressingByteSource` worker for
     * @p originalPath after the sniff has decided it is
     * compressed. Wires up the progress dialog, poll timer, and
     * watcher; the worker runs on the Qt thread pool. Called
     * from `StreamNextPendingFile`; the finished slot
     * (`OnDecompressionFinished`) picks the flow back up on the
     * GUI thread.
     * @param originalPath The `originalPath` value.
     * @param codec The `codec` value.
     */
    void BeginAsyncDecompression(const QString &originalPath, loglib::internal::DecompressingByteSource::Codec codec);

    /**
     * @brief Continuation of `StreamNextPendingFile` for compressed
     * inputs: takes ownership of the worker-produced
     * `DecompressingByteSource`, resumes the parse (or the queue
     * drain, on failure) on the GUI thread.
     */
    void OnDecompressionFinished();

    /**
     * @brief Continues a prepared file open on the GUI thread.
     * @param originalPath The user-facing source path used for labels and persistence.
     * @param effectivePath The path probed and mapped by the parser.
     * @param decompressionAnchor Shared ownership that keeps a temporary decompressed file alive.
     * @return `true` when a parse worker starts; `false` after recording a synchronous open error.
     *
     * The function constructs the log source, detects its format, and begins or
     * appends streaming. A null anchor denotes an uncompressed input.
     */
    [[nodiscard]] bool ContinueOpenAfterPrepared(
        const QString &originalPath,
        const std::filesystem::path &effectivePath,
        std::shared_ptr<loglib::internal::DecompressingByteSource> decompressionAnchor
    );

    /**
     * @brief Shows per-tab decompression progress and starts the poll timer.
     */
    void ShowDecompressionProgress();

    /**
     * @brief Refreshes per-tab decompression progress and the optional window summary.
     *
     * Each in-flight decompression updates its own view. The window dialog
     * mirrors the selected tab when that tab is decompressing.
     */
    void UpdateDecompressionProgressUi();

    /**
     * @brief Stops the decompression poll timer and hides the window summary.
     */
    void TeardownDecompressionProgress();

    /**
     * @brief Cancels the selected tab's decompression, if any.
     *
     * Safe when no decompression is running.
     */
    void CancelInFlightDecompression();

    /**
     * @brief Cancels and drains a decompression on one session.
     * @param origin Session whose decompression should stop; ignored when null.
     *
     * Re-enables only that session's view and leaves sibling decompressions running.
     */
    void CancelInFlightDecompressionFor(LogSession *origin);

    /**
     * @brief Runs the `OnStreamingFinished` teardown when the queue drains
     * through a decompression that did NOT hand off to a parse
     * worker (e.g. the last queued file failed decompression).
     * Without this, error buckets accumulate silently and
     * `mSessionMode` stays `Static` with no live worker, leaving
     * the config UI greyed out until the user forces a reset.
     *
     * No-op while another worker is still in flight -- the natural
     * drain point (`OnStreamingFinished` or the next
     * `OnDecompressionFinished`) will run instead. Rows +
     * `mCurrentSource` are preserved; auto-save runs if the
     * surviving session shape is restorable.
     */
    void FinalizeAfterDecompressionIfChainTerminal();

    /**
     * @brief Slot for `LogModel::streamingFinished`. Hoisted out of an
     * inline lambda so crash-dump frames identify it by name and
     * tests can exercise the post-streaming reset logic directly.
     * Owns queue draining, session-mode reset, auto-save publish,
     * and parse-error surfacing.
     * @param result The operation result.
     */
    void OnStreamingFinished(StreamingResult result);

    /**
     * @brief Appends a titled error batch to the appropriate session.
     * @param title The error-group title.
     * @param errors The errors to append.
     * @param originatingSession The owning session, or `nullptr` for the active session.
     */
    void ShowParseErrors(
        const QString &title, const std::vector<std::string> &errors, LogSession *originatingSession = nullptr
    );

    /**
     * @brief Pop a warning dialog summarising filters dropped on load.
     * Records @p droppedCount for tests and skips the modal when
     * `mSuppressDialogsForTest` is set.
     * @param droppedCount The `droppedCount` value.
     * @param message The message text.
     */
    void ShowDroppedFiltersDialog(int droppedCount, const QString &message);

    /**
     * @brief Add @p filter to `mSimpleLeaves` and build its menu entry.
     * Bulk callers pass `deferSync = true` and run one trailing
     * mirror + `UpdateFilters` after the loop.
     * @param id The `id` value.
     * @param filter The `filter` value.
     * @param deferSync The `deferSync` value.
     */
    void AddLogFilter(const QString &id, const loglib::LeafRule &filter, bool deferSync = false);

    /**
     * @brief Display title for @p filter (e.g. `info, warn` for enum,
     * `[1.5, 2.0]` for a numeric range). Shared by the Filters
     * menu and the column-header right-click menu.
     * @param filter The `filter` value.
     * @return The result described above.
     */
    [[nodiscard]] QString BuildFilterTitle(const loglib::LeafRule &filter) const;

    /**
     * @brief Recompile `LogConfiguration::expression` against the
     * current column layout and install it on the proxy. Called
     * after every mutation and on column/enum-column changes.
     */
    void UpdateFilters();

    /**
     * @brief True iff the window is worth auto-saving: history manager
     * attached, `File`-kind source with at least one locator, and
     * a static (re-openable) session. Live-tail / stream sessions
     * can't be restored from a JSON snapshot (the producer is
     * stateful), so we skip them. Takes the just-finished mode
     * explicitly because `streamingFinished` resets `mSessionMode`
     * to `Idle` before the auto-save hook runs.
     * @param justFinishedMode The `justFinishedMode` value.
     * @return The result described above.
     */
    [[nodiscard]] bool ShouldAutoSaveSession(SessionMode justFinishedMode) const;

    /**
     * @brief Drop `mAutoSaveUuid` from the persisted open-windows set and
     * clear the field. Called from every state-discarding path so
     * the next AutoSave produces a fresh entry instead of
     * overwriting the previous session's JSON.
     */
    void DetachAutoSaveUuid();

    /**
     * @brief Snapshot `mSimpleLeaves` (ordered by `mSimpleLeafOrder`),
     * proxy sort, and `mCurrentSource` into the configuration.
     * Simple-mode leaves become the top-level `And` children;
     * pre-existing non-Leaf siblings (Advanced-editor output) are
     * preserved. Bulk callers set `deferSync = true` and mirror
     * once at the end.
     */
    void MirrorSessionStateToConfiguration();

    /**
     * @brief Shared tail of `OpenLogStream` and `OpenLogStreamForTest`:
     * runs the actual open (producer construction, flush-and-reset
     * of the previous session, BeginStreaming) on @p file. Pulled
     * out so tests can drive the post-dialog path without a modal
     * `QFileDialog`.
     * @param file The file path to use.
     */
    void OpenLogStreamFromPath(const QString &file);

    /**
     * @brief Attach the pending live tail after its static prefix drains.
     */
    void ContinueLiveTailAfterPrefix();

    /**
     * @brief Global rotation-history preference after applying the CLI
     * override. Session-level gating is handled separately.
     * @return The result described above.
     */
    [[nodiscard]] bool ShouldAutoDetectRotationHistory() const;

    /**
     * @brief Rotation-history preference after global, CLI, and session gates.
     * @return The result described above.
     */
    [[nodiscard]] bool EffectiveAutoDetectRotationHistory() const;

    /**
     * @brief Clear expansion-undo state at a destructive session boundary.
     */
    void ClearRotationExpansionUndoState() noexcept;

    /**
     * @brief Clear a pending historical-prefix promotion so it cannot
     * attach to a later session.
     */
    void ClearPendingLiveTailPromotion() noexcept;

    /**
     * @brief Controls whether expansion consults the current source's
     * opt-out and locator deduplication state.
     */
    enum class RotationSourceGating
    {
        /**
         * @brief Apply both the session opt-out and loaded-locator deduplication.
         */
        HonourAll,
        /**
         * @brief Ignore the outgoing source during a destructive replacement.
         */
        Ignore,
        /**
         * @brief Apply the session opt-out but ignore stale locator keys.
         */
        HonourOptOutOnly,
    };

    /**
     * @brief Expand rotation families in oldest-first order and deduplicate
     * them according to @p gating. Bundles pass through unchanged.
     * @p addedOut counts only auto-discovered paths absent from the
     * input. @p primaryOut receives the first expanded family's
     * canonical primary, or remains empty when nothing was added.
     * @param logPaths The `logPaths` value.
     * @param addedOut The `addedOut` value.
     * @param gating The `gating` value.
     * @param primaryOut The `primaryOut` value.
     * @return The result described above.
     */
    [[nodiscard]] QStringList ExpandLogPathsWithRotationSiblings(
        const QStringList &logPaths, int &addedOut, RotationSourceGating gating, QString *primaryOut = nullptr
    ) const;

    /**
     * @brief Report an expansion and enable its Undo action.
     * @param addedCount The `addedCount` value.
     * @param primary The `primary` value.
     */
    void ShowRotationHistoryToast(int addedCount, const QString &primary);

    /**
     * @brief Reopen the original inputs without rotation expansion.
     */
    void UndoRotationExpansion();

    /**
     * @brief Sync the Settings action to the effective preference.
     */
    void SyncRotationHistoryActionCheckedState();

    /**
     * @brief Persist the preference and mirror it to the current source.
     * @param enabled The new enabled state.
     */
    void OnRotationHistoryPrefToggled(bool enabled);

    /**
     * @brief Mirrors session state and writes the configuration slice
     * selected by @p scope.
     * @param path The destination path.
     * @param scope The persistence scope.
     * @throws std::exception When the configuration cannot be written.
     */
    void DoSaveConfiguration(const QString &path, loglib::SaveScope scope);

    /**
     * @brief Parses and applies a configuration, resetting the model,
     * validating saved filters, and restoring sort. A successful load
     * detaches `mAutoSaveUuid`; callers that need the previous UUID must
     * explicitly re-pin it.
     * @param path The configuration path.
     * @return `true` on success; `false` on a parse error.
     */
    bool DoLoadConfiguration(const QString &path);

    /**
     * @brief Apply an already-parsed `LogConfiguration` to the live model.
     * Shared tail of `DoLoadConfiguration`. Destructive: clears
     * proxy rules + sort, resets the model, replaces the
     * configuration, and rebuilds filters. Returns false (with a
     * warning dialog) if the apply step throws.
     * @param parsed The `parsed` value.
     * @return The result described above.
     */
    bool ApplyLoadedConfiguration(loglib::LogConfiguration parsed);

    /**
     * @brief Re-validate every saved filter against the freshly-loaded
     * columns and revive survivors via `AddLogFilter`. Shared by
     * `DoLoadConfiguration` and `TryLoadAsConfiguration`.
     */
    void RebuildFiltersFromConfiguration();

    /**
     * @brief Drop simple-mode leaves, per-filter menu entries, and the
     * "Clear All Filters" gate. Does *not* touch
     * `LogConfiguration::expression`, mark dirty, or refresh the
     * mirror/indicators -- callers handle those (bulk load runs one
     * mirror at the end; `ClearAllFilters` runs its own refresh).
     * Split out so `ClearAllFilters` can additionally reset the
     * expression tree while the load path preserves it.
     */
    void ResetSimpleFilterState();

    /**
     * @brief Gate "Clear All Filters" on
     * `LogConfiguration::expression` (not `mSimpleLeaves`) so an
     * Advanced-only tree still enables the escape hatch. Must run
     * after `MirrorSessionStateToConfiguration`.
     */
    void SyncClearAllFiltersEnabled();
    void ApplyTableStyleSheet();

    /**
     * @brief Pick the light- or dark-variant title-bar icon to match the
     * active theme.
     */
    void ApplyThemedWindowIcon();

    /**
     * @brief Slot for `ThemeControl::themeChanged()`. Re-applies the
     * table QSS and repaints the viewport so cells re-query
     * `data()` for the new per-level brushes / fonts.
     */
    void OnThemeChanged();

    /**
     * @brief Canonical `EnumDictionary` for @p columnIndex; nullptr when the
     * column is not promoted or has no keys.
     * @param columnIndex The logical column index.
     * @return The result described above.
     */
    [[nodiscard]] const loglib::EnumDictionary *ResolveEnumDictionary(int columnIndex) const;

    /**
     * @brief True iff every selected string in @p filter resolves to an id
     * in the canonical dictionary. Gates whether an
     * `enumColumnsChanged` tick triggers a filter-rule rebuild.
     * @param filter The `filter` value.
     * @return The result described above.
     */
    [[nodiscard]] bool EnumFilterFullyResolved(const loglib::LeafRule &filter) const;

    /**
     * @brief Applies and clears a deferred configuration sort when still valid.
     *
     * User sorting, an invalid column, or a clear latch causes no change. Deferral
     * avoids sorted per-row insertion while streaming the restored source.
     */
    void ApplyDeferredSortFromConfig();

    void SetConfigurationUiEnabled(bool enabled);
    void UpdateStreamingStatus();

    /**
     * @brief Posts a status message for the session currently aliased as active.
     * @param message The message text.
     * @param timeoutMs The display timeout in milliseconds.
     *
     * Background-tab completions queue the message on that session and show it
     * when the tab becomes selected.
     */
    void PostStatusMessage(const QString &message, int timeoutMs);

    /**
     * @brief Starts the elapsed-time timer and 1 Hz refresh tick for live tail.
     */
    void StartLiveTailTicker();

    /**
     * @brief Stops the 1 Hz tick but keeps the elapsed value for the final status.
     */
    void StopLiveTailTicker();

    /**
     * @brief Opens (or raises) the modeless shortcuts dialog, building it lazily.
     */
    void ShowShortcutsDialog();

    /**
     * @brief Persists window geometry and dock layout to `QSettings`.
     */
    void SaveWindowChrome() const;

    /**
     * @brief Restores window geometry and dock layout from `QSettings`.
     * Must run after every dock/toolbar widget has its `objectName`
     * so `restoreState` can resolve them.
     */
    void RestoreWindowChrome();

    /**
     * @brief Rebuilds the window title from the current session state.
     */
    void UpdateWindowTitle();

    /**
     * @brief Marks the active session's filters dirty.
     *
     * Configuration loading suppresses the change; successful transitions refresh
     * the title through `filtersDirtyChanged`.
     */
    void MarkFiltersDirty();

    /**
     * @brief Last-used dialog directory, or the platform `Documents` location
     * on first run. Persisted in `QSettings` under `ui/lastOpenDir`.
     * @return The result described above.
     */
    [[nodiscard]] QString DefaultOpenDir() const;

    /**
     * @brief Persists the directory of @p path as the last-used dialog dir.
     * @param path The path to use.
     */
    void RememberLastOpenDir(const QString &path);

    /**
     * @brief Last-used export directory (`ui/lastExportDir`), kept
     * separate from `ui/lastOpenDir` so a one-off export to a
     * shared drive doesn't retarget the next `File -> Open...`
     * dialog. Falls back to `DefaultOpenDir()` on first use.
     * @return The result described above.
     */
    [[nodiscard]] QString DefaultExportDir() const;

    /**
     * @brief Persists the directory of @p path under `ui/lastExportDir`.
     * @param path The path to use.
     */
    void RememberLastExportDir(const QString &path);

    /**
     * @brief Appends shortcut text to each action's tooltip and mirrors the
     * tooltip into `statusTip()`. Skips actions whose tooltip already
     * names the shortcut, so it's safe to re-run.
     */
    void FinaliseActionMetadata();

    /**
     * @brief Build the persistent primary `QToolBar` and `insertToolBar`
     * it ahead of `mStreamToolbar` so the two bars share the top
     * dock area as one strip (main first, stream second). Tags
     * every populated action with a `svgIconPath` property (and,
     * where applicable, `svgIconPathChecked` for a distinct
     * On-state glyph) so `RefreshThemedIcons` can re-tint without
     * a per-action switch. Called once at the end of the
     * constructor, after `mStreamToolbar`, `mActionToggleFind`
     * and `mActionToggleAnchors` are wired (every action the
     * builder references must already exist) but before
     * `RestoreWindowChrome` reads the persisted dock state.
     */
    void BuildMainToolbar();

    /**
     * @brief Re-render every themed icon at the current palette
     * `WindowText` colour and device-pixel ratio. Walks
     * `mThemedActions` (every entry was registered with its
     * preferred anchor widget at `BuildMainToolbar` time) and
     * reads the `svgIconPath` / `svgIconPathChecked` properties
     * each action carries. Actions without the property are
     * skipped, so it's safe to run before `BuildMainToolbar`
     * (no-op when `mMainToolbar` is still null) and idempotent
     * under duplicate triggers (theme switch + DPR change firing
     * within the same event loop pass).
     *
     * Anchor-driven (not toolbar-iteration-driven) so actions
     * reached through `QToolBar::addWidget` (the open-stream
     * split button's default action, its popup-menu entries)
     * participate in the refresh -- those are wrapped in an
     * internal `QWidgetAction` that does NOT appear in the
     * toolbar's `actions()` list and would otherwise be missed,
     * leaving the split button blank on theme flip.
     */
    void RefreshThemedIcons();

    /**
     * @brief Repopulate the Add-filter split-button dropdown with one
     * `Add filter on "<col>"…` entry per *visible* column.
     * Connected to the menu's `aboutToShow` so the listing
     * always reflects the current configuration without us
     * having to invalidate it from every column-mutation site
     * (`SetColumnVisible`, `OnSourceColumnsMoved`,
     * `ColumnsManagerDialog::Accept`, post-stream column
     * promotion, …). The clicked entry routes through the same
     * `AddFilter(uuid, nullopt, openEditor=true, initialColumn=idx)`
     * path the header right-click uses, so column reorders
     * between menu build and click resolve via the stable `keys`
     * captured in the lambda.
     *
     * Hidden columns are skipped (`SetInitialColumn` refuses to
     * preselect them, mirroring the header context menu) and
     * each entry is disabled when the model has no rows
     * (`AddFilter` would short-circuit with a status-bar hint).
     * An empty configuration produces a single disabled
     * `(no columns yet)` placeholder so the dropdown is never
     * silently empty.
     * @param menu The menu to populate.
     */
    void RebuildAddFilterMenu(QMenu *menu);

    /**
     * @brief Repopulate `menuSort`: `actionClearSort` + separator,
     * then two checkable rows per visible column
     * (`▲ "<col>"` / `▼ "<col>"`) whose check state mirrors
     * the proxy's current sort. Hooked to `aboutToShow` so the
     * menu always reflects the live configuration.
     */
    void RebuildSortMenu();

    /**
     * @brief Repopulate the Sort split-button dropdown with the same
     * per-column rows as `RebuildSortMenu`, minus the
     * Clear-sort row (the toolbar has its own Clear-Sort button
     * next to this one).
     * @param menu The menu to populate.
     */
    void RebuildSortByMenu(QMenu *menu);

    /**
     * @brief Append two checkable rows per visible column to @p menu
     * (`▲ "<col>"` / `▼ "<col>"`) whose check state mirrors
     * the proxy's sort. The triangle is the same glyph
     * `QHeaderView` paints as its sort indicator. Rows are
     * disabled when the model has no rows or the column's data
     * doesn't match its configured type; disabled rows carry a
     * tooltip pointing at Configuration Diagnostics. Shared
     * core for `RebuildSortMenu` and `RebuildSortByMenu`;
     * returns true if any row was added (so the caller can
     * fall back to a placeholder when every column is hidden).
     * @param menu The menu to populate.
     * @return The result described above.
     */
    bool AppendSortByEntries(QMenu *menu);

    /**
     * @brief `AppendSortByEntries` plus the placeholder fallbacks both
     * Sort surfaces share: `(no columns yet)` for an empty
     * configuration and `(every column is hidden ...)` when
     * nothing visible remains.
     * @param menu The menu to populate.
     */
    void AppendSortEntriesOrPlaceholder(QMenu *menu);

    /**
     * @brief Sync `actionClearSort`'s enabled state and
     * `mClearSortStatusButton`'s visibility with the proxy's
     * current sort. Hidden when the model is empty; visible
     * while a sort is active. Hooked to `layoutChanged`, the
     * source's row signals, and horizontal `headerDataChanged`
     * (so a column rename refreshes the tooltip without
     * waiting for the next sort / filter event).
     */
    void UpdateSortStatus();

    /**
     * @brief Repopulate the Clear-filters split-button dropdown with
     * one `Remove "<col>": <title>` entry per active filter,
     * grouped by column index then sorted by display title.
     * Connected to the menu's `aboutToShow`; we don't have to
     * invalidate it from `AddLogFilter` / `ClearFilter` /
     * `ClearAllFilters` because the menu is rebuilt every time
     * it's opened.
     *
     * When `mSimpleLeaves` is empty the menu shows a single disabled
     * `(no filters)` placeholder so the dropdown surfaces a
     * hint instead of opening blank. (The button's default
     * action stays gated by `actionClearAllFilters->setDisabled`
     * in the empty-filters branch; on the styles where the
     * menu arrow shares the disabled state with the button face,
     * the empty dropdown remains unavailable because there is
     * nothing to remove.)
     * @param menu The menu to populate.
     */
    void RebuildClearFiltersMenu(QMenu *menu);

    /**
     * @brief Snapshot active filter titles per column from `mSimpleLeaves`
     * and push them into `LogModel::SetColumnFilterDetails`,
     * which drives the funnel decoration + "Filters:" tooltip
     * section. Sorts each column's titles for stable display.
     *
     * Called from every `mSimpleLeaves` mutation point and from
     * column-shape signals that can shift a column's resolved
     * index. Idempotent via the model-side diff guard.
     */
    void SyncColumnFilterIndicators();

    /**
     * @brief Re-evaluate the stream toolbar's visibility against the current
     * session mode.
     */
    void UpdateStreamToolbarVisibility();

    /**
     * @brief Scroll to the newest row when Follow newest is on. Thin
     * gate that forwards to `JumpToNewestRow`, which is what
     * actually handles the proxy chain and the filtered fallback.
     */
    void ScrollToNewestRowIfFollowing();

    /**
     * @brief Scroll to the newest row through the proxy chain, ignoring
     * session mode / `actionFollowTail`. Used by the pill click
     * ("catch me up") rather than the streaming-policy state
     * machine. Safe to call with no rows.
     *
     * Target resolution:
     *   1. Map the source-newest row through the proxy chain.
     *      Correct under custom column sorts.
     *   2. If filtered out, walk source rows backwards up to
     *      `JUMP_FALLBACK_WALK_LIMIT` and take the first that
     *      survives the proxy.
     *   3. If nothing visible, snap to the proxy's visual tail
     *      (`LogTableView::GetTailEdge()`) so the click always
     *      moves the viewport.
     */
    void JumpToNewestRow();

    /**
     * @brief Re-apply the persisted retention cap to the model.
     */
    void ApplyStreamingRetention();

    /**
     * @brief Connect the current selection model to the Record Details refresh
     * slot. Must be re-called after any `setModel` on the table view --
     * Qt destroys the old selection model and severs prior connections.
     * Uses `Qt::UniqueConnection`, so repeat calls are idempotent.
     */
    void RebindRecordDetailSelectionTracking();

    /**
     * @brief True iff the find dock is realised and visible. Tabified-dock
     * semantics: the inactive tab of a tabified group reports
     * `isVisible() == false`, which is exactly what we want -- no
     * match-count recount when the indicator can't be seen. The null
     * check guards construction and shutdown races.
     * @return The result described above.
     */
    [[nodiscard]] bool IsFindBarVisible() const noexcept
    {
        return mFindDock != nullptr && mFindDock->isVisible();
    }

    /**
     * @brief True iff the find dock is visible AND holds keyboard focus.
     * Used by the parse-errors auto-raise guard and `Find()`'s smart
     * toggle (Ctrl+F closes only when focus is already in the bar).
     * @return The result described above.
     */
    [[nodiscard]] bool FindBarHoldsFocus() const noexcept
    {
        return IsFindBarVisible() && mFindDock->isAncestorOf(QApplication::focusWidget());
    }

    /**
     * @brief Wire the standard dock toggle pattern: `action->toggled` opens
     * (`onShow`, default show+raise) or closes (`close()`) the dock;
     * `visibilityChanged(true)` re-checks the action; `closedSignal`
     * un-checks it on genuine dismissal. Splitting on/off across
     * these two signals is what lets the menu checkmark survive
     * tab switches in a tabified group.
     *
     * `onShown` runs after the action is re-checked and is the hook
     * for per-dock catch-up work (selection refresh, match count, ...).
     * @param dock The `dock` value.
     * @param action The `action` value.
     * @param closedSignal The `closedSignal` value.
     * @param onShow The `onShow` value.
     * @param onShown The `onShown` value.
     */
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

    /**
     * @brief One guarded record per tab. Order matches `mTabWidget`; tab
     * closure schedules both the session and view for deletion.
     */
    struct WindowTab
    {
        SessionInstanceId id;
        QPointer<LogSession> session;
        QPointer<LogSessionView> view;
        /**
         * @brief Last-focused child, or null until the tab has held focus.
         */
        QPointer<QWidget> lastFocus;
        /**
         * @brief Subscriptions that remain live while this tab is backgrounded.
         *
         * Cleared before the tab leaves `mTabs` so queued completions cannot
         * mutate an unhosted session or a surviving sibling.
         */
        ScopedConnections persistentConnections;
    };

    /**
     * @brief Central tab widget. Qt owns each view page; sessions are
     * children of the window. Null before tab setup completes.
     */
    QTabWidget *mTabWidget = nullptr;

    /**
     * @brief One record per tab in `mTabWidget`, in tab-index order.
     * Kept in sync with `mTabWidget->currentIndex()` via
     * `OnActiveTabChanged`. Reordering rewrites the vector via
     * `QTabBar::tabMoved` so a `SessionInstanceId`->index lookup
     * stays correct.
     */
    std::vector<std::unique_ptr<WindowTab>> mTabs;

    /**
     * @brief Stable identity persisted for workspace restore and window routing.
     * Lazily allocated by `WorkspaceWindowUuid()`.
     * `mutable` because `CaptureWorkspaceWindow()` is const
     * but the read seeds the identity on demand.
     */
    mutable QString mWorkspaceWindowUuid;

    /**
     * @brief Guard for `OnActiveTabChanged` re-entrancy during tab
     * construction / destruction (Qt fires `currentChanged` when
     * we add or remove pages before the vectors are consistent).
     */
    bool mSuppressActiveTabChange = false;

    /**
     * @brief True while completion handling is temporarily origin-bound to a
     * background tab. Prevents shell-global UI from changing for that tab.
     */
    bool mBackgroundCompletionInFlight = false;

    /**
     * @brief Non-owning active-tab alias. Each session is parented to this
     * window and tracked by its corresponding tab record.
     */
    LogSession *mSession = nullptr;

    /**
     * @brief Guarded active-tab view alias. The tab widget owns each view
     * and `QPointer` clears during deferred deletion.
     */
    QPointer<LogSessionView> mSessionView;

    /**
     * @brief Connections whose endpoint or callback depends on active-session
     * aliases. Cleared and rebuilt atomically on each tab switch.
     */
    ScopedConnections mSessionConnections;

    /**
     * @brief Non-owning aliases into the active session's model objects. Each points at a
     * `QObject` that `mSession` constructs and reaps in reverse
     * order via `~LogSession()`; the aliases are cached in the
     * window ctor so the shell body reads `mModel->` instead of
     * `mSession->Model()->` on hot paths.
     *
     * Lifetime for the window body: the aliases are valid from
     * the end of the ctor until the ``~MainWindow()`` body
     * finishes. The destructor uses them (``mModel->Reset()``
     * under a ``SessionSwitchScope``) before ``mSession`` itself
     * is destroyed.
     *
     * Lifetime past ``~MainWindow()`` body: Qt6's
     * ``QObjectPrivate::deleteChildren`` walks direct children in
     * forward registration order. The ctor body creates them in
     * this order:
     *
     *     1. ``centralWidget`` is created by ``ui->setupUi(this)``
     *        (first line of the ctor body), and later reparented
     *        to host ``mSessionView`` + the view's children
     *        (``mTableView``, ``mOverviewRailWidget``,
     *        ``mOverviewRailModel``).
     *     2. ``mSession`` is created immediately after
     *        ``setupUi`` returns (``new LogSession(..., this)``),
     *        so it registers as the *second* direct child of
     *        ``MainWindow``.
     *     3. Docks (``mAnchorsDock``, ``mHistogramDock``,
     *        ``mFindDock``, ``mParseErrorsDock``,
     *        ``mRecordDetailDock``, ...) are constructed further
     *        down the ctor body, so they register *after*
     *        ``mSession``.
     *
     * Forward-order reap is therefore:
     *
     *     1. ``~centralWidget`` -> ``~mSessionView`` -> view
     *        child sweep destroys ``mTableView``,
     *        ``mOverviewRailWidget``, ``mOverviewRailModel`` (and
     *        every widget the view owns).
     *     2. ``~mSession`` -> the model quintet
     *        (``AnchorManager``, ``HighlightRuleSet``,
     *        ``LogModel``, ``RowOrderProxyModel``,
     *        ``LogFilterModel``) is destroyed here, in the
     *        reverse order ``LogSession``'s ctor constructed
     *        them.
     *     3. Docks / dialogs registered under ``this`` are
     *        reaped in their own registration order
     *        (``mAnchorsDock``, ``mHistogramDock``,
     *        ``mParseErrorsDock``, ``mFindDock``, ...) --
     *        **after** ``mSession`` has already destroyed the
     *        quintet.
     *
     * Consumer QObjects that hold non-owning raw pointers into
     * the quintet split into two categories:
     *
     *   * **Reaped before ``mSession`` (safe by construction)**:
     *     the view subtree (``mTableView``, ``mOverviewRailModel``,
     *     ``mOverviewRailWidget``), because ``centralWidget`` sits
     *     ahead of ``mSession`` in the registration order.
     *     ``mLevelCellDelegate`` is parented on ``mTableView`` so
     *     it also falls in this bucket.
     *   * **Reaped after ``mSession`` (safe only via Qt's
     *     disconnect machinery)**: every dock plus every dialog
     *     parented directly on ``this``. Their compiler-generated
     *     destructors must not dereference ``mModel`` /
     *     ``mAnchors`` / ``mHighlights`` / ``mRowOrderProxyModel``
     *     / ``mSortFilterProxyModel``, because those objects have
     *     already been destroyed by ``~mSession``. Today this
     *     works because (a) the docks' destructors are
     *     compiler-generated no-ops for the raw-pointer members,
     *     and (b) ``QObject::destroyed`` auto-disconnects any
     *     lingering signal wires when the quintet dies during
     *     step 2. Adding an explicit ``disconnect(mModel, ...)``
     *     or any dereference to a dock
     *     destructor must either move that dock ahead of
     *     ``mSession`` in the registration order or hold the
     *     pointer as ``QPointer<LogModel>`` so the deref becomes
     *     a null-check post-``~mSession``. See
     *     ``LogSession::RowOrderProxy`` / ``FilterProxy`` /
     *     ``Model``.
     *
     * The destructor's explicit teardown ordering (drain workers
     * under a ``SessionSwitchScope``, reset via ``mModel->Reset()``,
     * clear ``mSessionConnections``) all runs inside the
     * ``~MainWindow()`` *body* before ``QObject``'s child sweep
     * starts, so the aliases are still valid there. It is the
     * dock destruction that has to defer to Qt's disconnect
     * machinery.
     */
    RowOrderProxyModel *mRowOrderProxyModel = nullptr;
    LogFilterModel *mSortFilterProxyModel = nullptr;
    /**
     * @brief Non-owning alias into `mSessionView->TableView()`. The table
     * widget is a child of `mSessionView`, not
     * the shell; teardown flows `~MainWindow` → central widget
     * destroy → `~LogSessionView` → child sweep reaches the
     * table. Non-null after ctor.
     */
    LogTableView *mTableView = nullptr;
    LogModel *mModel = nullptr;
    /**
     * @brief Icon-pill delegate for the level column. Owned via Qt
     * parentage; `nullptr` in the no-theme test fixture path
     * (icon mode is skipped there).
     */
    class LevelCellDelegate *mLevelCellDelegate = nullptr;

    // `LogSessionView` owns the installed-column state and enforces the
    // detach-before-reinstall-on-a-new-column invariant now; the
    // shell just forwards through `ApplyLevelCellDelegate`.
    /**
     * @brief Dockable find bar (owned via `QMainWindow` parentage).
     * `mFindRecord` is the hosted widget. `QPointer` on both so
     * model / proxy signals that fire during shutdown find them
     * null instead of dangling.
     */
    QPointer<FindDock> mFindDock;
    QPointer<FindRecordWidget> mFindRecord;
    /**
     * @brief Dockable replacement for the old `QMessageBox::warning`
     * parse-error popups. Hidden by default; auto-raised on the
     * first error of a session.
     */
    QPointer<ParseErrorsDock> mParseErrorsDock;
    /**
     * @brief Toggle action for `mFindDock`, mirrored onto the View menu.
     * Programmatic because the .ui has no entry.
     */
    QAction *mActionToggleFind = nullptr;
    /**
     * @brief Toggle action for `mParseErrorsDock`.
     */
    QAction *mActionToggleParseErrors = nullptr;

    /**
     * @brief Tab management actions. Declared programmatically
     * so the .ui file does not have to carry them: they belong on
     * the File menu (New Tab, Close Tab) and window (Next / Prev
     * Tab, with `Ctrl+Tab` shortcuts that Qt would swallow if
     * bound to a .ui action).
     */
    QAction *mActionNewTab = nullptr;
    QAction *mActionCloseTab = nullptr;
    QAction *mActionNextTab = nullptr;
    QAction *mActionPreviousTab = nullptr;
    QAction *mActionOpenInNewTab = nullptr;

    /**
     * @brief Checkable rotation-history Settings action.
     */
    QAction *mActionAutoDetectRotationHistory = nullptr;

    /**
     * @brief Enabled while the current session can undo its expansion.
     */
    QAction *mActionUndoRotationExpansion = nullptr;
    /**
     * @brief Status-bar indicator that surfaces when the parse-errors dock
     * has entries; clicking it opens the dock.
     */
    QPushButton *mParseErrorsStatusButton = nullptr;

    /**
     * @brief Cap on `sortedRows` (the vector powering the "*i* of *N*"
     * binary search). Past this many hits the scan may early-exit
     * once the rail's presence fold is settled, keeping the GUI
     * bounded on huge tables with a common needle. When
     * `overflowed` is set, `totalMatches` is a lower bound and
     * the position lookup degrades for match `#10 001` or later.
     */
    // Keep the short shell-local spelling for the session-owned cap.
    static constexpr int MAX_FIND_MATCH_COUNT = LogSession::MAX_FIND_MATCH_COUNT;

    // Keep the short shell-local spelling for the session-owned cache type.
    using FindMatchCache = LogSession::FindMatchCache;

    PreferencesEditor *mPreferencesEditor;

    /**
     * @brief Modeless editor for the merged regex-template catalog
     * (`Settings -> Regex templates...`). Created lazily on first
     * menu activation and reused across show/hide so in-flight
     * edits survive a close-reopen. Parented to `this` (Qt-owned).
     * Stays null when `mRegexTemplateRegistry` is null (the menu
     * action is disabled in that case).
     */
    RegexTemplatesEditor *mRegexTemplatesEditor = nullptr;

    /**
     * @brief Modeless editor for user-defined highlight rules
     * (`Settings -> Highlight rules...`). Lazy-construct /
     * survive-close, like the regex editor. `QPointer` so a
     * teardown-time delete zeroes the slot even while queued
     * signals from the rule set are in flight.
     */
    QPointer<HighlightRulesEditor> mHighlightRulesEditor;
    /**
     * @brief Non-owning. Lives in `main()` (or the test fixture).
     * `nullptr` for no-argument construction; theme code paths
     * in this class check before dereferencing.
     */
    ThemeControl *mTheme;

    /**
     * @brief Non-owning alias into `mSession->Anchors()`. The `AnchorManager` itself is owned
     * by `mSession` and reaped in reverse order via
     * `~LogSession()` so `~LogModel` runs while its non-owning
     * back-pointer is still valid. Non-null after construction.
     * See the model-quintet aliases block above for the full
     * lifetime discussion and the caveat for consumer QObjects
     * registered after `mSession`.
     */
    AnchorManager *mAnchors = nullptr;

    /**
     * @brief Non-owning alias into `mSession->Highlights()`. Runtime companion to
     * `LogConfiguration::highlightRules`. Constructed before
     * `mModel` inside `LogSession`'s ctor initializer list so
     * the model can hold a non-owning pointer for the paint
     * cascade. Same lifetime story as `mAnchors`.
     */
    HighlightRuleSet *mHighlights = nullptr;

    /**
     * @brief Owned. Hidden by default; toggled via View -> Anchors.
     */
    AnchorsDock *mAnchorsDock = nullptr;

    /**
     * @brief Toggle action for the Anchors dock. Re-added to View on every
     * `RebuildViewMenu`. Programmatic because the .ui has no entry.
     */
    QAction *mActionToggleAnchors = nullptr;

    /**
     * @brief Owned. Bottom-docked histogram strip; hidden by default,
     * toggled via View -> Histogram (or Ctrl+H).
     */
    HistogramDock *mHistogramDock = nullptr;

    /**
     * @brief Toggle action for the Histogram dock; re-added on every
     * `RebuildViewMenu`. Programmatic because the .ui has no entry.
     */
    QAction *mActionToggleHistogram = nullptr;

    /**
     * @brief Non-owning alias into `mSessionView->OverviewRailModelPtr()`.
     * The bucket model is a child of `mSessionView`,
     * not the shell. Kept alive even when the rail is hidden so
     * the toggle is instant.
     */
    OverviewRailModel *mOverviewRailModel = nullptr;

    /**
     * @brief Non-owning alias into `mSessionView->OverviewRail()`.
     * Constructed as a child of `mSessionView` so it dies
     * with the tab.
     *
     * Visibility changes preserve the required reparenting:
     * when visible, `LogTableView::AttachOverviewRail` reparents
     * the widget INTO the table view (it lives inside the table's
     * reserved right viewport margin); when hidden,
     * `SetOverviewRailVisible(false)` reparents it back onto
     * `mSessionView`, preventing the widget from being orphaned
     * when its tab closes. The widget is intentionally not placed in
     * `LogSessionView`'s `QVBoxLayout`; the layout stays a
     * single-child stack of the table view. `QPointer` so a
     * teardown-time delete zeroes the slot; the slot is null
     * after `~LogSessionView` runs.
     */
    QPointer<OverviewRailWidget> mOverviewRailWidget;

    /**
     * @brief Checkable toggle for the overview rail, mirrored onto the
     * primary toolbar and the View menu. Persisted through
     * `ui/showOverviewRail`.
     */
    QAction *mActionToggleOverviewRail = nullptr;

    /**
     * @brief Anchor hotkey actions: index N maps to `Ctrl+(N+1)`.
     * `mActionClearRowAnchor` is `Ctrl+0`; jumps are `F2` /
     * `Shift+F2`; edit-note is `F4`; clear-all is `Ctrl+Shift+A`.
     * Registered via `addAction` so they fire even without menu
     * placement.
     */
    std::array<QAction *, loglib::ANCHOR_PALETTE_SIZE> mAnchorColorActions{};
    QAction *mActionClearRowAnchor = nullptr;
    QAction *mActionJumpNextAnchor = nullptr;
    QAction *mActionJumpPrevAnchor = nullptr;
    QAction *mActionEditRowAnchorNote = nullptr;
    QAction *mActionClearAllAnchors = nullptr;
    /**
     * @brief Simple-mode leaves and their display order now live on
     * `mSession`; MainWindow methods reach them
     * through `mSession->SimpleLeaves()` /
     * `mSession->MutableSimpleLeaves()` /
     * `mSession->SimpleLeafOrder()` /
     * `mSession->MutableSimpleLeafOrder()`. The public `Filters()`
     * accessor keeps its shape for tests.
     */

    /**
     * @brief Status-bar label shown while a streaming session is active.
     */
    QLabel *mStatusLabel = nullptr;

    /**
     * @brief Status-bar label that reads "*n* of *m* shown" while a
     * filter is hiding rows, or "*m* lines" otherwise. Hidden
     * when the source model is empty. Updated via
     * `UpdateRowsShownStatus` from source / proxy row signals.
     */
    QLabel *mRowsShownLabel = nullptr;

    /**
     * @brief Status-bar button that triggers `actionClearAllFilters`.
     * Visible only when at least one filter is active and the
     * source model has rows. Mirrors the UX of
     * `mDiagnosticsButton` / `mParseErrorsStatusButton`.
     */
    QPushButton *mClearFiltersStatusButton = nullptr;

    /**
     * @brief Status-bar button bound to `actionClearSort`. Visible
     * only while a sort is active and the source has rows.
     * Same flat / clickable styling as
     * `mClearFiltersStatusButton`.
     */
    QPushButton *mClearSortStatusButton = nullptr;

    /**
     * @brief Status-bar button showing the per-column type-mismatch
     * summary. Hidden when zero columns are mismatched; opens the
     * diagnostics dialog on click.
     */
    QPushButton *mDiagnosticsButton = nullptr;

    /**
     * @brief Lazy-owned diagnostics dialog; survives close so a second
     * open reuses the same window.
     */
    QPointer<class ConfigurationDiagnosticsDialog> mDiagnosticsDialog;

    /**
     * @brief The session that owned the model this diagnostics dialog is
     * bound to. Captured when the dialog is (re)opened
     * and consulted by `RebindSharedDocks` -- when the active
     * session changes to a different one, we close the dialog
     * rather than let it silently retain a stale model pointer.
     * `QPointer` so a session torn down out-of-order zeroes the
     * alias and the "same session" check degrades to false, which
     * still closes the dialog cleanly.
     */
    QPointer<LogSession> mDiagnosticsDialogSession;

    /**
     * @brief Lazy-owned bulk column manager dialog; survives close so a
     * second open reuses the same window.
     */
    QPointer<class ColumnsManagerDialog> mColumnsManagerDialog;

    /**
     * @brief Originating session for `mColumnsManagerDialog`;
     * same semantics as `mDiagnosticsDialogSession` above.
     */
    QPointer<LogSession> mColumnsManagerDialogSession;

    /**
     * @brief Originating session for `mHighlightRulesEditor`.
     * Same semantics as `mDiagnosticsDialogSession` above --
     * consulted by `RebindSharedDocks` to close the editor when
     * the active session changes to a different one, so the
     * `rulesSaved` fan cannot land on the wrong session's model.
     */
    QPointer<LogSession> mHighlightRulesEditorSession;

    /**
     * @brief Dock pane that follows the selected row. Hidden until opened
     * via the View menu or a double-click. `QDockWidget` provides
     * the float / dock / close chrome.
     */
    RecordDetailDock *mRecordDetailDock = nullptr;

    /**
     * @brief Last QSS pushed to the table body / header. Compared on
     * re-apply so we can skip unchanged writes -- Qt re-polishes
     * the whole view on every `setStyleSheet`, even no-op ones.
     * We cache our own snapshot (not `widget->styleSheet()`) so
     * external writers can't trip the diff.
     */
    QString mLastBodyStyleSheet;
    QString mLastHeaderStyleSheet;

    /**
     * @brief One snapshot window plus the scoped `destroyed` connection
     * installed by `OpenRecordDetailWindow`. The scoped handle lets
     * `~MainWindow` disconnect only what we wired (a blanket
     * `disconnect` would catch unrelated hooks).
     */
    struct TrackedSnapshotWindow
    {
        QPointer<RecordDetailWindow> window;
        QMetaObject::Connection destroyedConnection;
    };

    /**
     * @brief Open snapshot windows keyed by the original heap address (cast
     * to `quintptr` for stable identity). Each window is
     * `Qt::WA_DeleteOnClose`; the `destroyed` lambda removes the
     * entry by id, so the map self-compacts without sweeps and
     * removal is unambiguous under concurrent teardown.
     */
    QHash<quintptr, TrackedSnapshotWindow> mRecordDetailWindows;

    /**
     * @brief Toolbar holding Pause/Follow tail/Stop; visible only during a
     * live-tail session.
     */
    QToolBar *mStreamToolbar = nullptr;

    /**
     * @brief Persistent primary toolbar (Open / Filter / View toggles /
     * Preferences). Inserted ahead of `mStreamToolbar` in the top
     * dock area, so the combined strip reads "Main | Stream"
     * left-to-right when both are visible. `QPointer` because
     * `RefreshThemedIcons` can be invoked during shutdown after
     * Qt has begun tearing down child widgets but before the
     * `MainWindow` destructor finishes; a dangling raw pointer
     * would crash on the next palette change. `objectName` is
     * `mainToolbar` so `restoreState` round-trips its dock area
     * and visibility.
     */
    QPointer<QToolBar> mMainToolbar;

    /**
     * @brief One themed action plus the widget that drives its tinting
     * policy (palette / iconSize / DPR). Used by
     * `RefreshThemedIcons` as the single registry of "actions
     * that need re-tinting on palette / theme / DPR change".
     *
     * The anchor is a hint, not an ownership relation: most
     * toolbar actions point at their host toolbar so the pixmap
     * is rasterised at the toolbar's exact `iconSize` (avoiding
     * downsample on platforms whose style reports a larger
     * `PM_LargeIconSize`). Actions reached only via menus
     * (`menuRecentSessions`) point at the window because there
     * is no toolbar to anchor against.
     *
     * Both fields are `QPointer` so an action / widget torn down
     * out of order during shutdown surfaces as null instead of
     * dangling.
     */
    struct ThemedActionEntry
    {
        QPointer<QAction> action;
        QPointer<QWidget> anchor;
    };

    /**
     * @brief Every action whose icon is generated by `icon_loader` and
     * needs re-tinting on palette / theme / DPR change.
     * Populated once by `BuildMainToolbar`; cleared on rebuild
     * for idempotency. Includes:
     *   * Main-toolbar actions (anchor = `mMainToolbar`).
     *   * Stream-toolbar actions (anchor = `mStreamToolbar`).
     *   * Open-stream split button's default + dropdown actions
     *     (anchor = `mMainToolbar`) -- these would be missed by
     *     a toolbar-iteration refresh because `addWidget` wraps
     *     the button in an internal `QWidgetAction` and the
     *     underlying action is not in `toolbar->actions()`.
     *   * `File -> Recent Sessions` submenu indicator (anchor =
     *     `this`) and other menu-only themed actions.
     */
    QList<ThemedActionEntry> mThemedActions;

    /**
     * @brief Ctrl+/ action that opens the shortcuts reference dialog.
     */
    QAction *mActionShowShortcuts = nullptr;

    /**
     * @brief Lazy-built shortcuts dialog; kept alive so reopening preserves geometry.
     */
    QPointer<class ShortcutsDialog> mShortcutsDialog;

    // Note: the wall-clock ``QElapsedTimer`` since the active
    // live-tail session started lives on `mSession`; see
    // `LogSession::LiveTailElapsedTimer`. The 1 Hz UI tick timer
    // below stays on the shell because rendering is a view concern.

    /**
     * @brief 1 Hz timer that refreshes the live-tail elapsed-time display.
     */
    QTimer *mLiveTailTickTimer = nullptr;

    /**
     * @brief The "filters dirty" marker (`[*]` in the window title) and the
     * re-entrancy gate the configuration-load path uses to coalesce
     * per-filter mutations live on `mSession`. The
     * window subscribes to `LogSession::filtersDirtyChanged` and
     * projects the value into `setWindowModified` from
     * `UpdateWindowTitle`.
     */

    /**
     * @brief The filename of the active stream lives on `mSession`.
     * Reach it through `mSession->StreamingFileName()`
     * / `mSession->SetStreamingFileName()` /
     * `mSession->ClearStreamingFileName()`.
     */

    /**
     * @brief Source descriptor lives on `mSession`. Reach it
     * through `mSession->CurrentSource()` /
     * `mSession->MutableCurrentSource()` /
     * `mSession->SetCurrentSource()` /
     * `mSession->ResetCurrentSource()`. Mirrored into
     * `LogConfiguration::source` before a `SaveScope::Full` save by
     * `MirrorSessionStateToConfiguration`.
     */

    /**
     * @brief Non-owning. Provided by `main()` for the production window;
     * `nullptr` for ad-hoc / test-only instances, in which case
     * auto-save / Recent Sessions / restore-on-launch are no-ops.
     */
    SessionHistoryManager *mHistoryManager = nullptr;

    /**
     * @brief Non-owning. Provided by `main()` so `OpenNetworkStream` can
     * pass it to `NetworkStreamDialog`. `nullptr` for ad-hoc
     * instances, in which case the dialog uses the library's
     * built-in template catalog only.
     */
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

    /**
     * @brief QFutureWatcher for the current async decompression. Owns a
     * `std::shared_ptr<DecompressingByteSource>`; the shared_ptr
     * is captured into the subsequent parse callable so the temp
     * file survives for the whole parse. `nullptr` when no
     * decompression is in flight.
     */
    // The QFutureWatcher lives on `mSession`;
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

    /**
     * @brief 200 ms timer that pumps decompression atomics into per-tab
     * progress strips and the optional window summary.
     */
    QTimer *mDecompressionPollTimer = nullptr;

    /**
     * @brief Optional window-level summary of the selected tab's decompression.
     *
     * Per-tab progress strips own cancellation and completion. `QPointer`
     * because `deleteLater` may run between the finished slot and
     * destruction.
     */
    QPointer<QProgressDialog> mDecompressionProgressDialog;

    // Note: `mDecompressionOriginalPath` (user-facing path being
    // decompressed), `mDecompressionCodecName` (pre-sniffed codec
    // label rendered by the poll-timer lambda), and
    // `mDecompressionStartedAt` (wall-clock start for the completion
    // toast) all live on `mSession`; see
    // `LogSession::DecompressionOriginalPath`.

    // --------------------------- Filtered-row export -----------------
    // Async orchestration for `File -> Export Filtered Rows...`.
    // Fresh `StopSource` per run, session-owned `QFutureWatcher<void>`,
    // per-tab progress strip, optional window summary dialog, and a
    // `QTimer` that polls export atomics on every hosted session.

    // The export QFutureWatcher lives on `mSession`; see
    // `LogSession::ExportWatcherPtr`. The shell still
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

    /**
     * @brief 200 ms poll timer that pumps export atomics into per-tab
     * progress strips and the optional window summary.
     */
    QTimer *mExportPollTimer = nullptr;

    /**
     * @brief Optional window-level summary of the selected tab's export.
     *
     * Per-tab progress strips own cancellation and completion. `QPointer`
     * because `deleteLater` may run between the finished slot and
     * destruction.
     */
    QPointer<QProgressDialog> mExportProgressDialog;

    // Note: `mExportDestinationPath` (user-facing destination),
    // `mExportFormatLabel` (human-readable format label), and
    // `mExportStartedAt` (wall-clock start for the toast) all live
    // on `mSession`; see `LogSession::ExportDestinationPath`.

    // Note: `mApplyEmbeddedBundleConfigForPath` (bundle path allowed
    // to apply embedded configuration) lives on `mSession`; see
    // `LogSession::ApplyEmbeddedBundleConfigForPath`.

    /**
     * @brief Kick off the async export worker. Models on
     * `BeginAsyncDecompression`.
     * @param plan The `plan` value.
     * @param destination The `destination` value.
     * @param formatLabel The `formatLabel` value.
     */
    void BeginAsyncExport(
        std::unique_ptr<slv::exports::ExportPlan> plan, const QString &destination, const QString &formatLabel
    );

    /**
     * @brief Kick off the async bundle-write worker. Mirrors
     * `BeginAsyncExport`: same in-flight guard and progress/cancel
     * plumbing, but the payload is `WriteSessionBundle` rather
     * than `RowExporter::Run`.
     * @param destination The `destination` value.
     * @param compressionLevel The `compressionLevel` value.
     * @param totalWorkers The `totalWorkers` value.
     */
    void BeginAsyncBundleExport(std::filesystem::path destination, int compressionLevel, int totalWorkers);

    /**
     * @brief Shows per-tab export progress and starts the poll timer.
     */
    void ShowExportProgress();

    /**
     * @brief Refreshes per-tab export progress and the optional window summary.
     *
     * Each in-flight export updates its own view. The window dialog
     * mirrors the selected tab when that tab is exporting.
     */
    void UpdateExportProgressUi();

    /**
     * @brief Stops the export poll timer and hides the window summary.
     */
    void TeardownExportProgress();

    /**
     * @brief Routes export completion to the session that owns the watcher.
     *
     * Direct test invocation without a sender falls back to the active session.
     */
    void OnExportFinished();

    /**
     * @brief Finalizes an export for the session that started it.
     * @param origin Session that started the export.
     *
     * Re-enables only the origin view. Background completion does not
     * change the selected tab's menus, docks, or status text.
     */
    void OnExportFinishedFor(LogSession *origin);

    /**
     * @brief Cancels the selected tab's export, if any.
     *
     * Safe when no export is running.
     */
    void CancelInFlightExport();

    /**
     * @brief Cancels and drains an export on one session.
     * @param origin Session whose export should stop; ignored when null.
     *
     * Re-enables only that session's view and leaves sibling exports running.
     */
    void CancelInFlightExportFor(LogSession *origin);

    // --------------------------- Session mode ------------------------

    /**
     * @brief `SessionMode` aliases `LogSession::Mode`. The current and
     * last terminal modes live on `mSession` and are
     * reached through `mSession->SessionMode()` /
     * `mSession->LastTerminalMode()` / `mSession->SetMode()`.
     * @return The result described above.
     */

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

    /**
     * @brief Streaming progress counters (`mStreamingLineCount`,
     * `mStreamingErrorCount`, `mStreamingErrorsCut`,
     * `mFirstStreamingBatchSeen`) and the `SourceStatus::Waiting`
     * latch (`mSourceWaiting`) live on `mSession`. Reach
     * them through `mSession->StreamingLineCount()` /
     * `SetStreamingLineCount()`, `StreamingErrorCount()` /
     * `SetStreamingErrorCount()`, `StreamingErrorsCut()` /
     * `SetStreamingErrorsCut()`, `FirstStreamingBatchSeen()` /
     * `SetFirstStreamingBatchSeen()`, and `IsSourceWaiting()` /
     * `SetSourceWaiting()`. `mSession->ResetStreamingProgress()`
     * covers the per-file start pattern (`line = error = 0;
     * firstBatchSeen = false`); `ResetStreamingCountersAndFileName()`
     * clears every field including the file name.
     */

    // The rotation flash state and timer are session-owned so one tab never projects
    // its flash onto another; the timer that clears it is
    // receiver-bound to the
    // session so teardown safely cancels the pending clear. See
    // `LogSession::TriggerRotationFlash()` /
    // `IsRotationFlashActive()`.

    /**
     * @brief Re-entrancy guard for `OnHeaderSectionMoved`: the slot
     * re-fires `sectionMoved` while resetting visual order, and
     * we swallow that volley.
     */
    bool mApplyingSectionMove = false;

    /**
     * @brief Re-entrancy guard for `enumColumnsChanged -> UpdateFilters`
     * lives on `mSession`. Reach it through
     * `mSession->IsApplyingEnumRebuild()` /
     * `mSession->SetApplyingEnumRebuild()`.
     */

    // Sticky Goto inputs live on `LogSessionView` because they are
    // view state; the shell's session-switch path clears
    // both through `mSessionView->ClearGotoStickyInputs()` and
    // the test seam `LastGotoTimestampInputForTest` forwards to
    // the view.

    /**
     * @brief The pending-apply-sort-from-config latch lives on `mSession`.
     * MainWindow call sites use
     * `mSession->HasPendingApplySortFromConfig()` /
     * `mSession->SetPendingApplySortFromConfig()`.
     */

    /**
     * @brief Latch held by the `SessionSwitchScope` RAII helper across a
     * destructive `mModel->Reset()` lives on `mSession`. Reach it through
     * `mSession->IsSessionSwitchInProgress()` /
     * `mSession->SetSessionSwitchInProgress()`.
     */

#ifdef LOGAPP_BUILD_TESTING
    /**
     * @brief Skip `ShowDroppedFiltersDialog`'s modal so a headless test
     * thread is not blocked.
     */
    bool mSuppressDialogsForTest = false;
    int mLastDroppedFilterCountForTest = 0;
    std::vector<ClosePromptChoiceForTest> mClosePromptChoicesForTest;
    bool mFailNextAutoSaveForTest = false;
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
