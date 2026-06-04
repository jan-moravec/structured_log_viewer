#pragma once

#include "anchor_manager.hpp"
#include "anchors_dock.hpp"
#include "find_record_widget.hpp"
#include "log_filter_model.hpp"
#include "log_model.hpp"
#include "log_table_view.hpp"
#include "preferences_editor.hpp"
#include "record_detail_dock.hpp"
#include "record_detail_window.hpp"
#include "row_order_proxy_model.hpp"

#include <loglib/log_configuration.hpp>
#include <loglib/theme.hpp>

// `loglib::EnumDictionary` is referenced via `ResolveEnumDictionary` below;
// the full type comes in transitively through `log_filter_model.hpp`.

#include <QAction>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QHash>
#include <QLabel>
#include <QList>
#include <QMainWindow>
#include <QMimeData>
#include <QPointer>
#include <QPushButton>
#include <QString>
#include <QStringList>
#include <QToolBar>
#include <QVBoxLayout>

#include <array>
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
QT_END_NAMESPACE

class SessionHistoryManager;
class ThemeControl;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
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

    /// Production constructor. The theme controller and history
    /// manager are owned by `main()`; the window keeps non-owning
    /// pointers and writes snapshots through the history manager
    /// on streaming completion / close. `theme` may be nullptr in
    /// tests; theme-dependent code paths fall back to defaults.
    MainWindow(ThemeControl *theme, SessionHistoryManager *historyManager, QWidget *parent = nullptr);

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

    /// The auto-save uuid pinned to this window, or empty if none.
    /// Used by `main()`'s `aboutToQuit` snapshot.
    [[nodiscard]] QString ActiveSessionUuid() const noexcept
    {
        return mAutoSaveUuid;
    }

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

    /// Live filter map; tests inspect it after a reorder.
    [[nodiscard]] const std::unordered_map<std::string, loglib::LogConfiguration::LogFilter> &Filters() const
    {
        return mFilters;
    }

    /// Owned `LogModel`; non-null after construction.
    [[nodiscard]] LogModel *Model() const
    {
        return mModel;
    }

    /// Owned `AnchorManager`; non-null after construction.
    [[nodiscard]] AnchorManager *Anchors() const noexcept
    {
        return mAnchors;
    }

    /// Select the next (forward=true) or previous anchored row in
    /// visible (proxy) order, honouring sort + filter + newest-first
    /// orientation. Wraps at the visible bounds. Surfaces a status-bar
    /// note when no anchored row is visible. Wired to F2 / Shift+F2.
    void JumpToAnchor(bool forward);

    /// Scroll to source row @p sourceRow and make it the sole
    /// selection. No-op on a negative row, unready model, or a row
    /// that is currently filtered out (the latter shows a status bar
    /// note). Used by the Anchors dock for jump targets.
    void SelectSourceRow(int sourceRow);

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
#endif

protected:
    bool event(QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

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

    /// Show the `ConfigurationDiagnosticsDialog` (constructed lazily).
    /// A second call raises the existing instance.
    void ShowConfigurationDiagnostics();

    /// Refresh the status-bar mismatch summary. Wired to
    /// `LogModel::columnHealthChanged`; hides the button when zero
    /// mismatches are present.
    void UpdateDiagnosticsStatus();

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
        const std::optional<loglib::LogConfiguration::LogFilter> &filter = std::nullopt,
        bool openEditor = true,
        int initialColumn = -1
    );
    void ClearAllFilters();
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

    /// `LogModel::columnsMoved` slot: remap `mFilters[*].row`,
    /// re-apply `Column::visible`, and refresh the proxy rules.
    /// Single source of truth for both header-drag and streaming-
    /// induced column moves (the latter is the timestamp bubble in
    /// `LogModel::AppendBatch`). The visibility re-apply is needed
    /// because Qt clears hidden flags via `initializeSections()`
    /// when the source has zero rows.
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
    /// Forward-declaration so the function signatures below can
    /// reference `SessionMode` before the full definition appears
    /// among the data members. The underlying type is pinned to
    /// match the definition.
    enum class SessionMode : int;

    /// RAII helper for the `mSessionSwitchInProgress` latch. Every
    /// destructive open path needs to flip the flag on, run a
    /// `mModel->Reset()` that synchronously emits
    /// `streamingFinished(Cancelled)`, then flip it back off once
    /// the new session is wired up. The RAII helper enforces the
    /// contract at the type level so no early-return path can
    /// forget the reset.
    struct SessionSwitchScope
    {
        explicit SessionSwitchScope(MainWindow &owner) noexcept
            : mOwner(owner)
        {
            mOwner.mSessionSwitchInProgress = true;
        }
        ~SessionSwitchScope()
        {
            mOwner.mSessionSwitchInProgress = false;
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

    /// Pop the next file off `mPendingOpenFiles` and parse it. Open
    /// errors accumulate in `mPendingOpenErrors`.
    void StreamNextPendingFile();

    /// Slot for `LogModel::streamingFinished`. Hoisted out of an
    /// inline lambda so crash-dump frames identify it by name and
    /// tests can exercise the post-streaming reset logic directly.
    /// Owns queue draining, session-mode reset, auto-save publish,
    /// and parse-error surfacing.
    void OnStreamingFinished(StreamingResult result);

    void ShowParseErrors(const QString &title, const std::vector<std::string> &errors);

    /// Pop a warning dialog summarising filters dropped on load.
    /// Records @p droppedCount for tests and skips the modal when
    /// `mSuppressDialogsForTest` is set.
    void ShowDroppedFiltersDialog(int droppedCount, const QString &message);

    /// Add @p filter to `mFilters` and build its menu entry. Pass
    /// `deferSync = true` from bulk callers
    /// (`RebuildFiltersFromConfiguration`) and run a single
    /// trailing mirror + `UpdateFilters` after the loop.
    void AddLogFilter(const QString &id, const loglib::LogConfiguration::LogFilter &filter, bool deferSync = false);

    /// Display title for @p filter (e.g. `info, warn` for an enum
    /// filter, `[1.5, 2.0]` for a numeric range). Shared between
    /// the Filters menu and the column-header right-click menu.
    [[nodiscard]] QString BuildFilterTitle(const loglib::LogConfiguration::LogFilter &filter) const;
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

    /// Snapshot `mFilters`, the proxy's sort, and `mCurrentSource`
    /// into the wire-format fields on the configuration. Filters
    /// are sorted by `(row, type, payload)` so two saves of an
    /// unchanged set produce byte-identical JSON. Bulk callers
    /// should `deferSync = true` and mirror once at the end.
    void MirrorSessionStateToConfiguration();

    /// Shared tail of `OpenLogStream` and `OpenLogStreamForTest`:
    /// runs the actual open (producer construction, flush-and-reset
    /// of the previous session, BeginStreaming) on @p file. Pulled
    /// out so tests can drive the post-dialog path without a modal
    /// `QFileDialog`.
    void OpenLogStreamFromPath(const QString &file);

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
    [[nodiscard]] bool EnumFilterFullyResolved(const loglib::LogConfiguration::LogFilter &filter) const;

    void SetConfigurationUiEnabled(bool enabled);
    void UpdateStreamingStatus();

    /// Re-evaluate the stream toolbar's visibility against the current
    /// session mode.
    void UpdateStreamToolbarVisibility();

    /// Scroll to the newest row when Follow tail is on; mapped through
    /// the proxy chain so the scroll lands correctly under a sort.
    void ScrollToNewestRowIfFollowing();

    /// Re-apply the persisted retention cap to the model.
    void ApplyStreamingRetention();

    /// Connect the current selection model to the Record Details refresh
    /// slot. Must be re-called after any `setModel` on the table view --
    /// Qt destroys the old selection model and severs prior connections.
    /// Uses `Qt::UniqueConnection`, so repeat calls are idempotent.
    void RebindRecordDetailSelectionTracking();

    Ui::MainWindow *ui;
    QVBoxLayout *mLayout;
    RowOrderProxyModel *mRowOrderProxyModel;
    LogFilterModel *mSortFilterProxyModel;
    LogTableView *mTableView;
    LogModel *mModel;
    FindRecordWidget *mFindRecord;
    PreferencesEditor *mPreferencesEditor;
    /// Non-owning. Lives in `main()` (or the test fixture).
    /// `nullptr` for legacy no-args construction; theme code paths
    /// in this class check before dereferencing.
    ThemeControl *mTheme;

    /// Owned. Brackets the lifetime of `mModel` and `mTableView`,
    /// both of which read from it. Non-null after construction.
    AnchorManager *mAnchors = nullptr;

    /// Owned. Hidden by default; toggled via View -> Anchors.
    AnchorsDock *mAnchorsDock = nullptr;

    /// Toggle action for the Anchors dock. Re-added to View on every
    /// `RebuildViewMenu`. Programmatic because the .ui has no entry.
    QAction *mActionToggleAnchors = nullptr;

    /// Anchor hotkey actions: index N maps to `Ctrl+(N+1)`.
    /// `mActionClearRowAnchor` is `Ctrl+0`; jumps are `F2` /
    /// `Shift+F2`; clear-all is `Ctrl+Shift+A`. Registered via
    /// `addAction` so they fire even without menu placement.
    std::array<QAction *, loglib::ANCHOR_PALETTE_SIZE> mAnchorColorActions{};
    QAction *mActionClearRowAnchor = nullptr;
    QAction *mActionJumpNextAnchor = nullptr;
    QAction *mActionJumpPrevAnchor = nullptr;
    QAction *mActionClearAllAnchors = nullptr;
    std::unordered_map<std::string, loglib::LogConfiguration::LogFilter> mFilters;

    /// Status-bar label shown while a streaming session is active.
    QLabel *mStatusLabel = nullptr;

    /// Status-bar button showing the per-column type-mismatch
    /// summary. Hidden when zero columns are mismatched; opens the
    /// diagnostics dialog on click.
    QPushButton *mDiagnosticsButton = nullptr;

    /// Lazy-owned diagnostics dialog; survives close so a second
    /// open reuses the same window.
    QPointer<class ConfigurationDiagnosticsDialog> mDiagnosticsDialog;

    /// Lazy-owned bulk column manager dialog; survives close so a
    /// second open reuses the same window.
    QPointer<class ColumnsManagerDialog> mColumnsManagerDialog;

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

    /// Filename of the active stream; empty when idle.
    QString mStreamingFileName;

    /// Source descriptor that matches what the model currently holds:
    /// file path for `File`, producer name for `NetworkStream`. Set
    /// on open and on session load; survives `Success` / `Cancelled`
    /// streaming finish (the rows are still there); cleared on
    /// `Failed` or by the next open's `Reset()`. Mirrored into
    /// `LogConfiguration::source` before a `SaveScope::Full` save.
    std::optional<loglib::LogConfiguration::Source> mCurrentSource;

    /// Non-owning. Provided by `main()` for the production window;
    /// `nullptr` for ad-hoc / test-only instances, in which case
    /// auto-save / Recent Sessions / restore-on-launch are no-ops.
    SessionHistoryManager *mHistoryManager = nullptr;

    /// uuid of the recents entry this window owns. Set after the
    /// first successful `WriteSnapshot` so subsequent saves rewrite
    /// the same JSON instead of appending one entry per save.
    QString mAutoSaveUuid;

    /// True iff `mAutoSaveUuid` is currently in `openWindowsAtQuit`.
    /// Lets `DetachAutoSaveUuid` skip the cross-process
    /// `RemoveOpenWindowUuid` round-trip when nothing was
    /// published. Must stay in lockstep with `AddOpenWindowUuid`
    /// call sites.
    bool mAutoSaveUuidPublished = false;

    /// Files queued by `StartStreamingOpenQueue`.
    QStringList mPendingOpenFiles;

    /// File-open errors collected while draining `mPendingOpenFiles`.
    std::vector<std::string> mPendingOpenErrors;

    /// Streaming session kind; gates UI variants. Set on open,
    /// cleared in `streamingFinished`. Underlying type pinned to
    /// match the forward declaration above.
    enum class SessionMode : int
    {
        Idle,
        Static,
        LiveTail,
    };
    SessionMode mSessionMode = SessionMode::Idle;

    /// Mirror of `mSessionMode` retained across `streamingFinished`
    /// (which resets `mSessionMode` to `Idle` before the auto-save
    /// hook runs). `closeEvent` -> `AutoSaveSessionSnapshot` reads
    /// this so a close after a finished live-tail correctly sees
    /// `LiveTail` (and bails) instead of `Idle`.
    SessionMode mLastTerminalSessionMode = SessionMode::Idle;

    [[nodiscard]] bool IsSessionActive() const noexcept
    {
        return mSessionMode != SessionMode::Idle;
    }
    [[nodiscard]] bool IsLiveTailSession() const noexcept
    {
        return mSessionMode == SessionMode::LiveTail;
    }

    /// Running line / error counts shown in the status bar.
    qsizetype mStreamingLineCount = 0;
    qsizetype mStreamingErrorCount = 0;

    /// True after the first non-empty batch; gates the one-shot column
    /// auto-resize.
    bool mFirstStreamingBatchSeen = false;

    /// True during the `— rotated` status-bar flash.
    bool mRotationFlashActive = false;

    /// Latched `SourceStatus::Waiting`; drives the `Source unavailable`
    /// status-bar variant.
    bool mSourceWaiting = false;

    /// Re-entrancy guard for `OnHeaderSectionMoved`: the slot
    /// re-fires `sectionMoved` while resetting visual order, and
    /// we swallow that volley.
    bool mApplyingSectionMove = false;

    /// Re-entrancy guard for `enumColumnsChanged -> UpdateFilters`.
    /// `UpdateFilters` rebuilds the proxy rules and re-asserts the
    /// model; an enum demote during the rebuild can re-fire the
    /// signal against half-updated state. The outer call finishes
    /// its rebuild and the queued re-entry becomes a no-op.
    bool mApplyingEnumRebuild = false;

    /// Latch held by the `SessionSwitchScope` RAII helper across a
    /// destructive `mModel->Reset()`. `OnStreamingFinished` short-
    /// circuits on the `Cancelled` branch when this is set, so the
    /// synchronous `streamingFinished(Cancelled)` emitted by `Reset()`
    /// does not run outgoing-session UI bookkeeping while the
    /// incoming session is being wired up.
    bool mSessionSwitchInProgress = false;

#ifdef LOGAPP_BUILD_TESTING
    /// Skip `ShowDroppedFiltersDialog`'s modal so a headless test
    /// thread is not blocked.
    bool mSuppressDialogsForTest = false;
    int mLastDroppedFilterCountForTest = 0;
#endif
};
