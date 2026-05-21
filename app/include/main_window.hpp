#pragma once

#include "find_record_widget.hpp"
#include "log_filter_model.hpp"
#include "log_model.hpp"
#include "log_table_view.hpp"
#include "preferences_editor.hpp"
#include "record_detail_dock.hpp"
#include "record_detail_window.hpp"
#include "row_order_proxy_model.hpp"

#include <loglib/log_configuration.hpp>

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

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    /// Selects how `StartStreamingOpenQueue` interacts with the
    /// currently-loaded state. `Append` is the default for the
    /// `OpenFiles` / drop entry points -- new files queue onto the
    /// active static session without clobbering its filters, sort,
    /// or rows. `Replace` matches the pre-append behaviour: reset
    /// the model, clear runtime filters, and drop the source
    /// descriptor before queueing the new files. Live-tail and
    /// network sessions ignore `Append` and behave as `Replace`
    /// (they are single-source by construction).
    enum class OpenMode
    {
        Append,
        Replace,
    };

    /// Backwards-compatible constructor: no history manager wired in,
    /// so auto-save / Recent Sessions / restore-on-launch behave as
    /// no-ops. Used by the existing test fixture and by ad-hoc
    /// MainWindow instantiations that don't care about history.
    MainWindow(QWidget *parent = nullptr);

    /// Production constructor used by `main()`. The history manager
    /// is owned by `main()` and lives for the application's lifetime;
    /// the window keeps a non-owning pointer and writes its session
    /// snapshot through it on streaming completion / window close.
    MainWindow(SessionHistoryManager *historyManager, QWidget *parent);

    ~MainWindow();

    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

    /// Restore the auto-saved session at @p jsonPath. Drives the same
    /// logic as the Recent Sessions reopen path, but starts from a
    /// JSON path so it can run before any menu wiring -- used by
    /// `main()`'s restore-on-launch flow and by tests. Skips the
    /// `NewSession` teardown (the window is freshly constructed and
    /// has nothing to discard).
    void RestoreLastSessionFromPath(const QString &jsonPath);

    void UpdateUi();

    /// Single sync point for newest-first display: picks the right
    /// `StreamingControl` flag for the active session mode and
    /// propagates it to the proxy, table view, and row colours.
    /// Idempotent.
    void ApplyDisplayOrder();

    /// Test-only `QAction` lookup by `objectName`; works around a Qt
    /// 6.8 + offscreen-QPA `findChild<QAction*>` bug.
    [[nodiscard]] QAction *FindUiAction(const QString &name) const;

    /// Test-only accessors for the filter pipeline. The same Qt 6.8 +
    /// offscreen-QPA traversal bug that motivates `FindUiAction` also
    /// strands `findChild<QMenu*>("menuFilters")` and `findChildren<
    /// QSortFilterProxyModel*>()` on the Linux runner; tests reach
    /// these owned objects through the explicit getters instead.
    [[nodiscard]] LogFilterModel *FilterModel() const
    {
        return mSortFilterProxyModel;
    }
    [[nodiscard]] QMenu *FiltersMenu() const;

    /// Test-only `View` menu accessor. Mirrors `FiltersMenu()`: the
    /// Qt 6.8 + offscreen-QPA `findChild<QMenu*>` traversal bug also
    /// strands `findChild<QMenu*>("menuView")` on the Linux runner.
    [[nodiscard]] QMenu *ViewMenu() const;

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

    /// Result of `BuildHeaderContextMenu`. `menu` is the caller-
    /// owned root menu; `filterSubMenus` is a non-owning test seam
    /// (the Linux Release offscreen-QPA toolchain strips `QMenu`
    /// metaobject hooks, so tests can't recover submenus by walking
    /// the tree). Production callers only need `menu`.
    struct HeaderContextMenu
    {
        QMenu *menu = nullptr;
        std::unordered_map<std::string, QMenu *> filterSubMenus;
    };

    /// Build the right-click header menu for @p logicalColumn.
    /// Caller owns `result.menu`. `result.menu` is null when
    /// @p logicalColumn is out of range.
    [[nodiscard]] HeaderContextMenu BuildHeaderContextMenu(int logicalColumn, QWidget *parent = nullptr);

    /// Live filter map; tests inspect it after a reorder.
    [[nodiscard]] const std::unordered_map<std::string, loglib::LogConfiguration::LogFilter> &Filters() const
    {
        return mFilters;
    }

    /// Test-only direct lookup for a per-filter sub-menu by id.
    /// Same Linux-Release-offscreen reason as `BuildHeaderContextMenu`'s
    /// out-parameter: walking `ui->menuFilters->actions()` and calling
    /// `QAction::menu()` -- or iterating `children()` and casting to
    /// `QMenu*` -- both return null on that toolchain even though the
    /// production code wires the submenu correctly. The map is
    /// maintained by `AddLogFilter` / `ClearFilter` / `ClearAllFilters`
    /// so the answer is the live submenu pointer.
    [[nodiscard]] QMenu *FilterSubMenu(const QString &filterID) const;

    /// Owned `LogModel`; non-null after construction.
    [[nodiscard]] LogModel *Model() const
    {
        return mModel;
    }

#ifdef LOGAPP_BUILD_TESTING
    /// Test-only direct accessor for the Record Details dock.
    /// Same Linux-Release-offscreen reason as `ViewMenu()` /
    /// `FilterSubMenu()`: `findChild<RecordDetailDock*>()` on the
    /// Linux Qt 6.8 + offscreen-QPA toolchain segfaults inside Qt's
    /// child-traversal even though the dock is correctly parented to
    /// `MainWindow` (production code never walks the tree this way).
    [[nodiscard]] RecordDetailDock *RecordDetailDockForTest() const
    {
        return mRecordDetailDock;
    }

    /// Test-only direct accessor for the central log table view.
    /// Same Linux-Release-offscreen reason as `RecordDetailDockForTest()`.
    [[nodiscard]] LogTableView *TableViewForTest() const
    {
        return mTableView;
    }

    /// Test-only snapshot-window list. Same Linux-Release-offscreen
    /// reason as `RecordDetailDockForTest()`: `findChildren` walks the
    /// child tree and that path is the unreliable one. The internal
    /// `QHash` is keyed by heap address; this view materialises the
    /// live `RecordDetailWindow*` set in insertion order so tests can
    /// observe both the count and the per-window state.
    [[nodiscard]] QList<RecordDetailWindow *> RecordDetailWindowsForTest() const;

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
    /// only updates the test counter (modals block the offscreen
    /// QPA test thread). Default false.
    void SetSuppressDialogsForTest(bool suppress);

    /// Filters dropped on the most recent
    /// `LoadConfigurationFromPathForTest` call. Reset on each load.
    [[nodiscard]] int LastDroppedFilterCountForTest() const;

    /// Test-only setter for `mCurrentSource`. Lets fixture-driven
    /// tests assert the descriptor round-trips through Save Session
    /// without running a real open path.
    void SetCurrentSourceForTest(std::optional<loglib::LogConfiguration::Source> source);

    /// Test-only direct accessor for the diagnostics status-bar
    /// button. `QObject::findChild<QPushButton*>("diagnosticsButton")`
    /// is unreliable on the GitHub-hosted Linux runner with Qt 6.8 +
    /// offscreen QPA (see `FindActionByObjectName` for the same
    /// workaround applied to QActions); this bypasses the lookup.
    [[nodiscard]] QPushButton *DiagnosticsButtonForTest() const noexcept
    {
        return mDiagnosticsButton;
    }

    /// Test-only entry into the queued static-files open path,
    /// bypassing the file dialog and the keyboard-modifier sniff.
    void OpenFilesForTest(const QStringList &files, OpenMode mode);

    /// Test-only entry into the Open-with-Configuration flow that
    /// bypasses the two file dialogs. Mirrors the production slot
    /// step-by-step: load the configuration via `DoLoadConfiguration`,
    /// then queue @p files via `StartStreamingOpenQueue` in `Append`
    /// mode. Returns false (and skips the queue) when the
    /// configuration fails to parse.
    bool OpenWithConfigurationForTest(const QString &configPath, const QStringList &files);
#endif

protected:
    bool event(QEvent *event) override;

private slots:
    /// Discard the current session and return to an empty view.
    /// Resets the model, runtime filters, source descriptor, and
    /// session mode. Bound to `actionNewSession` (Ctrl+N).
    void NewSession();
    /// Rebuild the `File -> Recent Sessions` submenu from the live
    /// `SessionHistoryManager` list. Connected to the submenu's
    /// `aboutToShow` so we never paint stale entries even after
    /// another window mutated the recents store.
    void RebuildRecentSessionsMenu();
    /// Reopen the recents entry @p uuid: load the per-uuid JSON via
    /// `DoLoadConfiguration` (restores columns / filters / sort /
    /// source descriptor), then queue the source's locators through
    /// `StartStreamingOpenQueue` in `Replace` mode so the active
    /// session is discarded before the recents session is restored.
    /// On success, `mAutoSaveUuid` is pinned to @p uuid so further
    /// edits update that recents entry instead of creating a new one.
    void OpenRecentSession(const QString &uuid);
    void OpenFiles();
    /// "Open with Configuration..." -- two-step prompt that first
    /// loads a configuration or session JSON (columns, filters, sort)
    /// and then opens the chosen log file(s) via `StartStreamingOpenQueue`
    /// in `Append` mode, so the freshly-loaded filters survive into
    /// the new session instead of being wiped by a destructive open.
    /// Bound to `actionOpenWithConfiguration`.
    void OpenWithConfiguration();
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
    void FilterTimeStampSubmitted(const QString &filterID, int row, qint64 beginTimeStamp, qint64 endTimeStamp);
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

    /// Rebuild the `View` menu on each `aboutToShow`. Each column
    /// gets a checkable action that toggles `Column::visible`.
    /// Always reachable, so it can restore visibility when every
    /// header section is hidden.
    void RebuildViewMenu();

private:
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

    /// Start a sequential streaming open of @p files.
    ///
    /// `OpenMode::Replace` is the historic behaviour: reset the model,
    /// clear runtime filters, drop `mCurrentSource`, then queue the
    /// files (the first uses `BeginStreaming`; subsequent files are
    /// dispatched through `AppendStreaming` on `streamingFinished`).
    ///
    /// `OpenMode::Append` keeps the active static session's filters,
    /// sort, source descriptor, and already-loaded rows intact and
    /// queues @p files onto the back of that session. With no active
    /// session it behaves like `Replace` minus the destructive reset --
    /// columns / filters that were just loaded via Open-with-
    /// Configuration survive into the new session. Live-tail and
    /// network sessions force `Replace` regardless of @p mode.
    void StartStreamingOpenQueue(QStringList files, OpenMode mode);

    /// Pop the next file off `mPendingOpenFiles` and parse it. Open
    /// errors accumulate in `mPendingOpenErrors`.
    void StreamNextPendingFile();

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

    /// Mirror the runtime session state into the model's
    /// configuration manager, then `WriteSnapshot` through the
    /// injected `SessionHistoryManager`. Reuses `mAutoSaveUuid` so
    /// the same window updates one recents entry across its lifetime
    /// instead of appending a fresh one on every save. No-op when
    /// the manager is null or there is no source descriptor.
    void AutoSaveSessionSnapshot();

    /// Snapshot `mFilters`, the proxy's sort, and `mCurrentSource`
    /// into the wire-format fields on the configuration. Filters
    /// are sorted by `(row, type, payload)` so two saves of an
    /// unchanged set produce byte-identical JSON. Bulk callers
    /// should `deferSync = true` and mirror once at the end.
    void MirrorSessionStateToConfiguration();

    /// Path-based save / load shared by the dialog slots and the
    /// test seams. `DoSaveConfiguration` mirrors session state and
    /// writes the slice selected by @p scope; throws on failure.
    /// `DoLoadConfiguration` resets the model, validates saved
    /// filters, restores sort, and returns false on parse error.
    void DoSaveConfiguration(const QString &path, loglib::SaveScope scope);
    bool DoLoadConfiguration(const QString &path);

    /// Re-validate every saved filter against the freshly-loaded
    /// columns and revive survivors via `AddLogFilter`. Shared by
    /// `DoLoadConfiguration` and `TryLoadAsConfiguration`.
    void RebuildFiltersFromConfiguration();
    void ApplyTableStyleSheet();

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
    std::unordered_map<std::string, loglib::LogConfiguration::LogFilter> mFilters;

    /// Per-filter `Filters` sub-menu pointers, keyed by filter id.
    /// Maintained alongside `mFilters`; the test-only `FilterSubMenu()`
    /// accessor reads from here because Qt 6.8 + offscreen QPA on the
    /// Linux Release toolchain strips the metaobject hooks that make
    /// `QAction::menu()` and `qobject_cast<QMenu*>(child)` work.
    std::unordered_map<std::string, QMenu *> mFilterSubMenus;

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
    /// `nullptr` for ad-hoc / test-only instantiations, in which case
    /// auto-save / Recent Sessions / restore-on-launch all degrade to
    /// no-ops.
    SessionHistoryManager *mHistoryManager = nullptr;

    /// uuid of the recents entry this window owns this session. Set
    /// after the first successful `WriteSnapshot` so subsequent
    /// auto-saves (and the closeEvent flush) rewrite the same JSON
    /// instead of appending a new entry for every save.
    QString mAutoSaveUuid;

    /// Files queued by `StartStreamingOpenQueue`.
    QStringList mPendingOpenFiles;

    /// File-open errors collected while draining `mPendingOpenFiles`.
    std::vector<std::string> mPendingOpenErrors;

    /// Streaming session kind; gates UI variants. Set on open, cleared
    /// in `streamingFinished`.
    enum class SessionMode
    {
        Idle,
        Static,
        LiveTail,
    };
    SessionMode mSessionMode = SessionMode::Idle;

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

#ifdef LOGAPP_BUILD_TESTING
    /// Skip `ShowDroppedFiltersDialog`'s modal so the test thread
    /// is not blocked under offscreen QPA.
    bool mSuppressDialogsForTest = false;
    int mLastDroppedFilterCountForTest = 0;
#endif
};
