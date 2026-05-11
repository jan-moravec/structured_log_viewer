#pragma once

#include "find_record_widget.hpp"
#include "log_filter_model.hpp"
#include "log_model.hpp"
#include "log_table_view.hpp"
#include "preferences_editor.hpp"
#include "row_order_proxy_model.hpp"

#include <loglib/log_configuration.hpp>

namespace loglib
{
class EnumDictionary;
}

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

    /// Test-only override of the internal session mode so display-order
    /// tests can exercise the `Static` branch without driving a real
    /// open flow.
    enum class TestSessionMode
    {
        Idle,
        Static,
        LiveTail,
    };
    void SetSessionModeForTest(TestSessionMode mode);

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

private:
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
    void AddLogFilter(const QString &id, const loglib::LogConfiguration::LogFilter &filter);
    void UpdateFilters();
    void ApplyTableStyleSheet();

    /// Resolve the canonical `EnumDictionary` for column @p columnIndex,
    /// or nullptr when the column is not yet promoted / has no keys.
    [[nodiscard]] const loglib::EnumDictionary *ResolveEnumDictionary(int columnIndex) const;

    /// True iff every selected string in @p filter currently resolves
    /// to an id in the canonical dictionary for its column. Used to
    /// decide whether an `enumColumnsChanged` tick warrants a rebuild
    /// of the proxy's filter rules.
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
    loglib::LogConfiguration mConfiguration;
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
};
