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

    /// Set @p logicalIndex's visibility. Mutates `LogConfiguration::
    /// Column::visible` and applies the flag via `QHeaderView::
    /// setSectionHidden`. Public so tests (and the View menu) can
    /// drive the flow without simulating a `QContextMenuEvent`.
    /// No-op for an out-of-range @p logicalIndex.
    void SetColumnVisible(int logicalIndex, bool visible);

    /// Reapply every column's `visible` flag to the horizontal header.
    /// Idempotent; invoked after column structural changes (load, reorder).
    void ApplyColumnVisibility();

    /// Force the horizontal header's visual-to-logical mapping back to
    /// identity (`visual == logical` for every section). Brackets the
    /// reordering with a `QSignalBlocker` so the no-op `sectionMoved`
    /// volley does not re-enter `OnHeaderSectionMoved`. Exposed so the
    /// out-of-band recovery path can call it without duplicating the
    /// loop. Idempotent.
    void ResetHeaderToIdentity();

    /// Build the right-click header context menu for the column at
    /// @p logicalColumn. Exposed publicly because the offscreen-QPA
    /// `findChild<QMenu*>` traversal bug (see `FiltersMenu()`) blocks
    /// the natural test path; callers own the returned menu. Not
    /// `const` because it wires `QObject::connect` for the menu's
    /// `Hide` / `Show column` actions.
    [[nodiscard]] QMenu *BuildHeaderContextMenu(int logicalColumn, QWidget *parent = nullptr);

    /// Read-only accessor for the per-filter map; tests use it to
    /// assert filter-row remap after a reorder.
    [[nodiscard]] const std::unordered_map<std::string, loglib::LogConfiguration::LogFilter> &Filters() const
    {
        return mFilters;
    }

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

    /// Test-only entry point to the production `TryLoadAsConfiguration`
    /// path used by single-file `OpenFiles`. Exposed because the real
    /// path is gated behind `QFileDialog`.
    bool TryLoadAsConfigurationForTest(const QString &file);

    /// Test-only entry point to the production
    /// `SetConfigurationUiEnabled` slot so the column-management gate
    /// (header drag + right-click menu) can be exercised without
    /// opening a real streaming session.
    void SetConfigurationUiEnabledForTest(bool enabled);

    /// Test-only entry points to the production `SaveConfiguration` /
    /// `LoadConfiguration` slots. Bypass the `QFileDialog` pop-up so
    /// the filter-persistence round-trip can be exercised headlessly.
    /// Both delegate to the same private helpers the dialog-driven
    /// slots use, so they exercise the eager-mirror + load-rebuild
    /// paths verbatim.
    void SaveConfigurationToPathForTest(const QString &path);
    void LoadConfigurationFromPathForTest(const QString &path);

    /// Test-only flag: when true, `ShowDroppedFiltersDialog` records
    /// the dropped count into `LastDroppedFilterCountForTest` instead
    /// of popping a modal `QMessageBox` (which would block the test
    /// thread under the offscreen QPA). Defaults to false; tests
    /// flip it on before driving a load.
    void SetSuppressDialogsForTest(bool suppress);

    /// Number of saved filters dropped on the most recent
    /// `LoadConfigurationFromPathForTest` call. Reset to 0 at the
    /// start of each load.
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
    void SaveConfiguration();
    void LoadConfiguration();

    void Find();
    void FindRecords(const QString &text, bool next, bool wildcards, bool regularExpressions);

    /// Add a filter rule, optionally opening the editor. Pass
    /// `openEditor = false` from the configuration-load path so a
    /// dropped saved filter does not pop the editor.
    void AddFilter(
        const QString &filterId,
        const std::optional<loglib::LogConfiguration::LogFilter> &filter = std::nullopt,
        bool openEditor = true
    );
    void ClearAllFilters();
    void ClearFilter(const QString &filterID);
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

    /// Translate a `QHeaderView::sectionMoved` (visual move) into a
    /// source-side `LogModel::MoveColumn` + filter-row remap, then
    /// reset the header's visual order so visual == logical again.
    void OnHeaderSectionMoved(int logicalIndex, int oldVisualIndex, int newVisualIndex);

    /// Build and show the header context menu at the click position.
    void ShowHeaderContextMenu(const QPoint &pos);

    /// Repopulate the `View` menu from the current configuration on
    /// each `aboutToShow`. Each column gets a checkable `QAction`
    /// whose `toggled` signal flips `Column::visible`. Reachable even
    /// when every header section is hidden (the only escape hatch in
    /// that state).
    void RebuildViewMenu();

private:
    /// Locate the column whose persisted `keys` exactly equals @p keys
    /// and return its current logical index, or `-1` if no such column
    /// exists. `keys` is the column's set of JSON keys (per
    /// `LogConfiguration::Column::keys`), which is the only identifier
    /// that survives a column reorder (logical indices shift; headers
    /// are user-visible and could legitimately duplicate). Used by the
    /// header / `View` menus so their lambdas resolve the right column
    /// even when a streaming-induced column move (e.g. timestamp
    /// bubbling) shifts indices between menu construction and trigger.
    [[nodiscard]] int FindColumnIndexByKeys(const std::vector<std::string> &keys) const;

    /// Display label for column @p columnIndex in the View / Hide /
    /// Show menus. Returns the column's `header` verbatim when it
    /// uniquely identifies the column, or `header [keys]` when the
    /// header collides with another column's header (Qt allows
    /// duplicate headers; `keys` is the stable identifier). Empty
    /// when @p columnIndex is out of range.
    [[nodiscard]] QString ColumnMenuLabel(size_t columnIndex) const;

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

    /// Show a single warning dialog summarising filters that were
    /// dropped on load. Built message is pre-rendered by the caller
    /// (the structured `FilterValidationFailure` type is a `.cpp`-
    /// local detail). Records @p droppedCount into the test-only
    /// counter and skips the modal when
    /// `mSuppressDialogsForTest` is set.
    void ShowDroppedFiltersDialog(int droppedCount, const QString &message);

    void AddLogFilter(const QString &id, const loglib::LogConfiguration::LogFilter &filter);
    void UpdateFilters();

    /// Snapshot the live `mFilters` map into the wire-format
    /// `LogConfiguration::filters` vector so `Save` and the lib-side
    /// `MoveColumn` filter-row remap operate on the current runtime
    /// state. Cheap; called eagerly from every `mFilters` mutation
    /// point. Order within the vector is unspecified (UUIDs are not
    /// persisted; menu ordering is rebuilt from the vector on load).
    void MirrorFiltersToConfiguration();

    /// Path-based save / load helpers shared by the dialog-driven
    /// `SaveConfiguration` / `LoadConfiguration` slots and the
    /// `LOGAPP_BUILD_TESTING` test seams. `DoSaveConfiguration`
    /// runs `MirrorFiltersToConfiguration` before delegating to the
    /// manager so persisted filters reflect the live UI; throws on
    /// I/O / serialisation failure (caller adapts to its own UX).
    /// `DoLoadConfiguration` resets the model, drops stale runtime
    /// filter state, validates each saved filter against the new
    /// column layout, and surfaces any drops through
    /// `ShowDroppedFiltersDialog`. Returns true on full success;
    /// false (with the failure already surfaced) on parse error.
    bool DoSaveConfiguration(const QString &path);
    bool DoLoadConfiguration(const QString &path);

    /// Drop runtime filter state, walk the freshly-loaded
    /// `LogConfiguration::filters` vector, validate each entry
    /// against the new column layout, and either revive the entry
    /// via `AddLogFilter` (with a fresh UUID) or accumulate a
    /// drop reason. Surfaces a single summary dialog if any
    /// filters were dropped. Shared by the full
    /// `DoLoadConfiguration` path and the speculative
    /// `TryLoadAsConfiguration` (single-file open / drop) path.
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

    /// Status-bar label shown while a streaming session is active.
    QLabel *mStatusLabel = nullptr;

    /// Toolbar holding Pause/Follow tail/Stop; visible only during a
    /// live-tail session.
    QToolBar *mStreamToolbar = nullptr;

    /// Filename of the active stream; empty when idle.
    QString mStreamingFileName;

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

    /// Re-entrancy guard for `OnHeaderSectionMoved`. The slot resets
    /// the header's visual order to identity after committing a
    /// source-side move; the resets fire `sectionMoved` again, which
    /// we swallow.
    bool mApplyingSectionMove = false;

#ifdef LOGAPP_BUILD_TESTING
    /// When true, `ShowDroppedFiltersDialog` skips the modal
    /// `QMessageBox::warning` and only updates
    /// `mLastDroppedFilterCountForTest`. Tests flip this on so
    /// `LoadConfigurationFromPathForTest` does not block.
    bool mSuppressDialogsForTest = false;
    int mLastDroppedFilterCountForTest = 0;
#endif
};
