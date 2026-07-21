#pragma once

#include "anchor_manager.hpp"
#include "anchors_dock.hpp"
#include "find_dock.hpp"
#include "find_record_widget.hpp"
#include "histogram_dock.hpp"
#include "log_filter_model.hpp"
#include "log_model.hpp"
#include "log_table_view.hpp"
#include "overview_rail_model.hpp"
#include "overview_rail_widget.hpp"
#include "parse_errors_dock.hpp"
#include "preferences_editor.hpp"
#include "record_detail_dock.hpp"
#include "record_detail_window.hpp"
#include "row_order_proxy_model.hpp"

#include <loglib/internal/decompressing_byte_source.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/stop_token.hpp>
#include <loglib/theme.hpp>

// `loglib::EnumDictionary` is referenced via `ResolveEnumDictionary` below;
// the full type comes in transitively through `log_filter_model.hpp`.

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

    /// Owned `HighlightRuleSet`; non-null after construction. Tests
    /// inspect it after column moves / rule saves to verify the
    /// rebind signal path.
    [[nodiscard]] HighlightRuleSet *Highlights() const noexcept
    {
        return mHighlights;
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
        return mDecompressionInFlight;
    }

    /// Test-only cancel injection: raises the same stop request
    /// `QProgressDialog::canceled` sends. Needed because the dialog
    /// is suppressed under `SetSuppressDialogsForTest`, making the
    /// production cancel wiring unreachable from tests. Callers must
    /// pump the event loop to drive the finished slot. No-op when
    /// no decompression is in flight.
    void RequestDecompressionCancelForTest()
    {
        if (mDecompressionInFlight)
        {
            mDecompressionStopSource.request_stop();
        }
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

    /// Push the current `mFindMatchCache` match state into the
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
        const std::optional<loglib::LogConfiguration::LogFilter> &filter = std::nullopt,
        bool openEditor = true,
        int initialColumn = -1
    );
    void ClearAllFilters();
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
    /// errors accumulate in `mPendingOpenErrors`; decompression
    /// failures land in `mPendingDecompressionErrors` (drained under
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
    /// (caller continues draining `mPendingOpenFiles`). This return
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

    /// Apply the saved sort from `mPendingApplySortFromConfig` to
    /// the view, then clear the latch. No-op when the latch is
    /// clear, when the user sorted mid-stream
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

    /// Marks filters as having unsaved edits and refreshes the title.
    /// No-op while `mLoadingConfiguration` is true so a config reload
    /// doesn't transiently flash `[*]`.
    void MarkFiltersDirty();

    /// Last-used dialog directory, or the platform `Documents` location
    /// on first run. Persisted in `QSettings` under `ui/lastOpenDir`.
    [[nodiscard]] QString DefaultOpenDir() const;

    /// Persists the directory of @p path as the last-used dialog dir.
    void RememberLastOpenDir(const QString &path);

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
    /// When `mFilters` is empty the menu shows a single disabled
    /// `(no filters)` placeholder so the dropdown surfaces a
    /// hint instead of opening blank. (The button's default
    /// action stays gated by `actionClearAllFilters->setDisabled`
    /// in the empty-filters branch; on the styles where the
    /// menu arrow shares the disabled state with the button face
    /// the dropdown won't open at all -- that's a graceful
    /// degradation, not a regression, since there's nothing to
    /// remove.)
    void RebuildClearFiltersMenu(QMenu *menu);

    /// Snapshot active filter titles per column from `mFilters` and
    /// push them into `LogModel::SetColumnFilterDetails`, which
    /// drives the funnel decoration + "Filters:" tooltip section.
    /// Sorts each column's titles for stable display.
    ///
    /// Called from every `mFilters` mutation point and from
    /// column-shape signals that can shift `filter.row`. Idempotent
    /// via the model-side diff guard.
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
    RowOrderProxyModel *mRowOrderProxyModel;
    LogFilterModel *mSortFilterProxyModel;
    LogTableView *mTableView;
    LogModel *mModel;
    /// Icon-pill delegate for the level column. Owned via Qt
    /// parentage; `nullptr` in the no-theme test fixture path
    /// (icon mode is skipped there).
    class LevelCellDelegate *mLevelCellDelegate = nullptr;

    /// Column the level delegate is currently installed on, or
    /// `-1` when detached. Stored so the next reapply can detach
    /// the old column before attaching the new one.
    int mInstalledLevelDelegateColumn = -1;
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
    /// Status-bar indicator that surfaces when the parse-errors dock
    /// has entries; clicking it opens the dock.
    QPushButton *mParseErrorsStatusButton = nullptr;

    /// Cap on `sortedRows` (the vector powering the "*i* of *N*"
    /// binary search). Past this many hits the scan may early-exit
    /// once the rail's presence fold is settled, keeping the GUI
    /// bounded on huge tables with a common needle. When
    /// `overflowed` is set, `totalMatches` is a lower bound and
    /// the position lookup degrades for match `#10 001` or later.
    static constexpr int MAX_FIND_MATCH_COUNT = 10000;

    /// Cached match state for the "*i* of *N*" indicator. Keyed by
    /// `(needle, wildcards, regex)` so Next / Previous can resolve
    /// the new `i` via binary search instead of re-scanning.
    ///
    /// - `sortedRows`: deduplicated, capped at `MAX_FIND_MATCH_COUNT`.
    /// - `totalMatches`: exact when `!overflowed`, otherwise a
    ///   lower bound. A cursor at match `#10 001` or later resolves
    ///   to `0` (no position highlight) under overflow.
    /// - `bucketCounts`: per-bucket totals mirrored to the rail
    ///   (empty when the rail had zero buckets during the scan).
    ///   Restored on find-dock reveal so a cache-hit recount can't
    ///   leave the rail on a top-biased strip. Presence-only —
    ///   density may be incomplete after an early-exit.
    struct FindMatchCache
    {
        QString needle;
        bool wildcards = false;
        bool regularExpressions = false;
        bool overflowed = false;
        std::vector<int> sortedRows;
        uint32_t totalMatches = 0;
        std::vector<uint32_t> bucketCounts;
    };
    std::optional<FindMatchCache> mFindMatchCache;
    PreferencesEditor *mPreferencesEditor;

    /// Modeless editor for the merged regex-template catalog
    /// (`Settings -> Regex templates...`). Created lazily on first
    /// menu activation and reused across show/hide so in-flight
    /// edits survive a close-reopen. Parented to `this` (Qt-owned).
    /// Stays null when `mRegexTemplateRegistry` is null (the menu
    /// action is disabled in that case).
    RegexTemplatesEditor *mRegexTemplatesEditor = nullptr;

    /// Modeless editor for user-defined highlight rules
    /// (`Settings -> Highlight rules...`). Same lazy-construct /
    /// survive-close pattern as the regex editor. `QPointer` so a
    /// teardown-time `delete` (Qt parentage) zeroes the slot even
    /// while queued signals from the rule set are in flight.
    QPointer<HighlightRulesEditor> mHighlightRulesEditor;
    /// Non-owning. Lives in `main()` (or the test fixture).
    /// `nullptr` for legacy no-args construction; theme code paths
    /// in this class check before dereferencing.
    ThemeControl *mTheme;

    /// Owned. Brackets the lifetime of `mModel` and `mTableView`,
    /// both of which read from it. Non-null after construction.
    AnchorManager *mAnchors = nullptr;

    /// Owned. Runtime companion to
    /// `LogConfiguration::highlightRules`: compiled predicates and
    /// per-row last-match cache. Constructed before `mModel` so the
    /// model can hold a non-owning pointer for the paint cascade.
    /// Non-null after construction.
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

    /// Bucketed index of the outermost proxy row space that feeds
    /// `mOverviewRailWidget`. Kept alive even when the rail is
    /// hidden so the toggle is instant.
    OverviewRailModel *mOverviewRailModel = nullptr;

    /// Parented on `mTableView` while visible (via
    /// `LogTableView::AttachOverviewRail`), on `this` while detached.
    /// `QPointer` so a teardown-time delete zeroes the slot.
    QPointer<OverviewRailWidget> mOverviewRailWidget;

    /// Checkable toggle for the overview rail, mirrored onto the
    /// primary toolbar and the View menu. Persisted through
    /// `ui/showOverviewRail`.
    QAction *mActionToggleOverviewRail = nullptr;

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

    /// Wall-clock since the active live-tail session started.
    QElapsedTimer mLiveTailTimer;

    /// 1 Hz timer that refreshes the live-tail elapsed-time display.
    QTimer *mLiveTailTickTimer = nullptr;

    /// True when filters have unsaved user edits; drives the `[*]` title marker.
    bool mFiltersDirty = false;

    /// Re-entrancy guard set while loading a config, so per-filter
    /// `AddLogFilter` calls don't mark the session dirty.
    bool mLoadingConfiguration = false;

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

    /// Non-owning. Provided by `main()` so `OpenNetworkStream` can
    /// pass it to `NetworkStreamDialog`. `nullptr` for ad-hoc
    /// instances, in which case the dialog uses the library's
    /// built-in template catalog only.
    RegexTemplateRegistry *mRegexTemplateRegistry = nullptr;

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
    /// Drained under the `tr("Error Opening File")` title.
    std::vector<std::string> mPendingOpenErrors;

    /// Decompression-specific errors collected while draining
    /// `mPendingOpenFiles`. Drained under
    /// `tr("Error Decompressing File")`; kept separate from
    /// `mPendingOpenErrors` so the batch is labelled correctly.
    /// User cancels surface as a status-bar toast instead (see
    /// `OnDecompressionFinished`).
    std::vector<std::string> mPendingDecompressionErrors;

    /// QFutureWatcher for the current async decompression. Owns a
    /// `std::shared_ptr<DecompressingByteSource>`; the shared_ptr
    /// is captured into the subsequent parse callable so the temp
    /// file survives for the whole parse. `nullptr` when no
    /// decompression is in flight.
    QFutureWatcher<std::shared_ptr<loglib::internal::DecompressingByteSource>> *mDecompressionWatcher = nullptr;

    /// Set in `BeginAsyncDecompression`, cleared in
    /// `OnDecompressionFinished` / `CancelInFlightDecompression`.
    /// Guards the finished slot against a queued callout event
    /// already dispatched onto the current stack before
    /// `setFuture({})` cleared the watcher's queue (e.g. via a
    /// nested event loop in a signal handler). Without this the
    /// slot would read `result()` off the empty future and splice
    /// a bogus "Failed to open ''" entry into the next session.
    bool mDecompressionInFlight = false;

    /// Stop source paired with the current decompression worker.
    /// `QProgressDialog::canceled` calls `request_stop()`; the
    /// worker polls `stop_requested()` between chunks. Refreshed
    /// per-open, so a cancelled operation cannot bleed into the
    /// next one.
    loglib::StopSource mDecompressionStopSource;

    /// Progress atomics: worker writes, GUI polls on the
    /// `mDecompressionPollTimer` cadence. Widened to `qint64` to
    /// match Qt's atomic contract; the payload never exceeds
    /// `size_t` in practice.
    QAtomicInteger<qint64> mDecompressionBytesIn = 0;
    QAtomicInteger<qint64> mDecompressionTotalBytesIn = 0;

    /// 200 ms cadence timer that pumps the atomics above into the
    /// progress dialog. Nulled out when no decompression is
    /// active.
    QTimer *mDecompressionPollTimer = nullptr;

    /// Modal-per-window progress dialog surfaced while a
    /// decompression is running. `QPointer` because `deleteLater`
    /// may run between the finished slot and the parent's
    /// destructor.
    QPointer<QProgressDialog> mDecompressionProgressDialog;

    /// User-facing path of the file being decompressed. Populated
    /// when the worker is dispatched so the progress dialog and
    /// completion toast can name the file even after the worker's
    /// local copy is gone.
    QString mDecompressionOriginalPath;

    /// Human-readable codec name (`gzip` / `bzip2` / `xz` / `zstd`)
    /// for the file being decompressed. Set up-front from the sniff
    /// so the poll-timer lambda can render the progress label
    /// without touching the worker's shared_ptr.
    QString mDecompressionCodecName;

    /// Wall-clock start of the current decompression, for the
    /// post-success status-bar toast.
    std::chrono::steady_clock::time_point mDecompressionStartedAt;

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

    /// High-water mark into `mModel->StreamingErrors()` consumed by
    /// the per-file batch in `OnStreamingFinished`. Multi-file static
    /// opens accumulate every file's errors in a single vector on
    /// the model; this watermark lets us peel off only the errors
    /// produced by the file that just finished so each file gets
    /// its own labelled batch in the `ParseErrorsDock`. Reset to 0
    /// alongside every `mParseErrorsDock->ResetSessionState()` to
    /// stay in lockstep with the model's `mStreamingErrors.clear()`.
    size_t mStreamingErrorsCut = 0;

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

    /// Latch: a loaded session's sort is pending, to be applied
    /// once streaming finishes. Avoids the O(N^2) per-row insert
    /// path that `LogFilterModel::OnSourceRowsInserted` falls into
    /// under an active sort (a 1 GB restore "never finishes"
    /// otherwise; pinned by
    /// `TestRestoreLastSessionDefersSortUntilStreamingFinishes`).
    ///
    /// Consumed by `OnStreamingFinished` (or by
    /// `StreamFromCurrentSourceOrSkip`'s early-return paths).
    /// `MirrorSessionStateToConfiguration` reads it so an
    /// auto-save mid-stream preserves the loaded sort instead of
    /// overwriting it with the proxy's transient `-1`.
    bool mPendingApplySortFromConfig = false;

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
