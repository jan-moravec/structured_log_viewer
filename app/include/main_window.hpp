#pragma once

#include "find_record_widget.hpp"
#include "log_filter_model.hpp"
#include "log_model.hpp"
#include "log_table_view.hpp"
#include "preferences_editor.hpp"
#include "row_order_proxy_model.hpp"

#include <loglib/log_configuration.hpp>

// `loglib::EnumDictionary` is referenced via `ResolveEnumDictionary` below;
// the full type comes in transitively through `log_filter_model.hpp`.

#include <QAction>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QLabel>
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

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

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
    /// that bypass the file dialog and run the same private helpers.
    /// `scope` defaults to `Full` so existing tests (written against
    /// the old single-action save that always wrote filters) keep
    /// passing; new tests should pass `SaveScope::ColumnsOnly` to
    /// exercise the "Save Configuration\u2026" path explicitly.
    void SaveConfigurationToPathForTest(const QString &path, loglib::SaveScope scope = loglib::SaveScope::Full);
    void LoadConfigurationFromPathForTest(const QString &path);

    /// When true, `ShowDroppedFiltersDialog` skips the modal and
    /// only updates the test counter (modals block the offscreen
    /// QPA test thread). Default false.
    void SetSuppressDialogsForTest(bool suppress);

    /// Filters dropped on the most recent
    /// `LoadConfigurationFromPathForTest` call. Reset on each load.
    [[nodiscard]] int LastDroppedFilterCountForTest() const;
#endif

protected:
    bool event(QEvent *event) override;

private slots:
    void OpenFiles();
    void OpenLogStream();
    /// Pop the `NetworkStreamDialog`, build the matching producer, and
    /// call `LogModel::BeginStreaming`.
    void OpenNetworkStream();
    /// "Save Configuration\u2026": writes only `columns` (the portable,
    /// data-source-independent layout).
    void SaveConfiguration();
    /// "Save Session\u2026": writes the full struct (columns + filters +
    /// sort + source).
    void SaveSession();
    /// Unified loader: detects whether the file carries session-only
    /// fields and applies whichever subset is present.
    void LoadConfiguration();

    /// Show the `ConfigurationDiagnosticsDialog`, constructing it
    /// lazily. Re-entrancy-safe: a second call raises the existing
    /// instance. Driven by the status-bar diagnostics button and the
    /// header right-click "Configuration Diagnostics\u2026" entry.
    void ShowConfigurationDiagnostics();

    /// Open the per-column editor dialog modally on @p columnIndex.
    /// No-op when @p columnIndex is out of range. Reached from the
    /// header right-click "Edit column \"X\"\u2026" entry and from
    /// double-clicking a row in the diagnostics dialog. The editor
    /// writes through `LogConfigurationManager`'s typed mutators on
    /// accept; the model side picks the changes up through the
    /// `headerDataChanged` and `columnHealthChanged` signals.
    void EditColumn(int columnIndex);

    /// Refresh the status-bar mismatch summary from the current
    /// `LogModel::ColumnHealth` snapshot. Wired to
    /// `LogModel::columnHealthChanged`; hides the button when no
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

    /// Reset state and start a sequential streaming open of @p files.
    /// The first file uses `BeginStreaming`; subsequent files are
    /// dispatched through `AppendStreaming` on `streamingFinished`.
    void StartStreamingOpenQueue(QStringList files);

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

    /// Snapshot the runtime session state (`mFilters`, the proxy's
    /// sort column/order, and `mCurrentSource`) into the wire-format
    /// fields on the configuration so `Save` and `MoveColumn`'s
    /// row-remap see the live set. Filters are sorted by
    /// `(row, type, payload)` so two consecutive saves of an
    /// unchanged set produce byte-identical JSON. O(N log N) in
    /// filter count; bulk callers should `deferSync = true` and
    /// mirror once at the end.
    void MirrorSessionStateToConfiguration();

    /// Path-based save / load helpers shared by the dialog-driven
    /// slots and the test seams. `DoSaveConfiguration` mirrors
    /// session state then writes the slice selected by @p scope;
    /// throws on I/O / serialisation failure. `DoLoadConfiguration`
    /// resets the model, validates each saved filter, surfaces
    /// drops via `ShowDroppedFiltersDialog`, restores any persisted
    /// sort, and returns false on parse error.
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

    /// Status-bar button that surfaces the per-column type-health
    /// summary computed by `LogModel::ColumnHealth`. Hidden when zero
    /// columns are mismatched; clicking it opens the diagnostics
    /// dialog (created lazily). Updated by `UpdateDiagnosticsStatus`,
    /// which is wired to `LogModel::columnHealthChanged`.
    QPushButton *mDiagnosticsButton = nullptr;

    /// Lazy-owned diagnostics dialog. Constructed on first
    /// `ShowConfigurationDiagnostics` call; survives close so a second
    /// open reuses the same window. Auto-refreshes on
    /// `columnHealthChanged`.
    QPointer<class ConfigurationDiagnosticsDialog> mDiagnosticsDialog;

    /// Toolbar holding Pause/Follow tail/Stop; visible only during a
    /// live-tail session.
    QToolBar *mStreamToolbar = nullptr;

    /// Filename of the active stream; empty when idle.
    QString mStreamingFileName;

    /// Descriptor of the source currently bound to the model:
    ///   - `kind == File`: full path (`StartStreamingOpenQueue` /
    ///     `OpenLogStream`).
    ///   - `kind == NetworkStream`: producer display name
    ///     (`OpenNetworkStream`).
    /// `nullopt` when no session is active. Mirrored into
    /// `LogConfiguration::source` by
    /// `MirrorSessionStateToConfiguration` before a `SaveScope::Full`
    /// save; reset by `ClearAllFilters`-style teardown paths.
    std::optional<loglib::LogConfiguration::Source> mCurrentSource;

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
