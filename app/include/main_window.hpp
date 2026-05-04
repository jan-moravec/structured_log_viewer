#pragma once

#include "find_record_widget.hpp"
#include "log_filter_model.hpp"
#include "log_model.hpp"
#include "log_table_view.hpp"
#include "preferences_editor.hpp"
#include "stream_order_proxy_model.hpp"

#include <loglib/log_configuration.hpp>

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

    /// Single sync point for newest-first display orientation. Reads
    /// `StreamingControl::IsNewestFirst()` once and propagates it to
    /// `StreamOrderProxyModel::SetReversed`, `LogTableView::SetTailEdge`,
    /// and `setAlternatingRowColors`, which all need to move together.
    /// Idempotent.
    void ApplyStreamingDisplayOrder();

protected:
    bool event(QEvent *event) override;

private slots:
    void OpenFiles();
    void OpenLogStream();
    void SaveConfiguration();
    void LoadConfiguration();

    void Find();
    void FindRecords(const QString &text, bool next, bool wildcards, bool regularExpressions);

    void AddFilter(
        const QString filterId, const std::optional<loglib::LogConfiguration::LogFilter> &filter = std::nullopt
    );
    void ClearAllFilters();
    void ClearFilter(const QString &filterID);
    void FilterSubmitted(const QString &filterID, int row, const QString &filterString, int matchType);
    void FilterTimeStampSubmitted(const QString &filterID, int row, qint64 beginTimeStamp, qint64 endTimeStamp);

    /// Pause / resume on the bridging sink. Bound to `actionPauseStream`.
    void TogglePauseStream(bool paused);

    /// Stop the active stream. Bound to `actionStopStream`.
    void StopStream();

    /// Rotation event from the source thread re-emitted on the GUI
    /// thread; flashes the `— rotated` suffix in the status bar.
    void OnRotationDetected();

    /// Source-thread status transition. Latches `mSourceWaiting` and
    /// refreshes the status bar so it shows `Source unavailable …`
    /// while the source is `Waiting`.
    void OnSourceStatusChanged(loglib::SourceStatus status);

private:
    /// Try to load @p file as a `LogConfiguration`; returns true and
    /// fires `UpdateUi` on success. Single-file open / drag-drop uses
    /// this to preserve the "drop a config file to load it" affordance.
    bool TryLoadAsConfiguration(const QString &file);

    /// Reset model + filter state and start a sequential streaming open
    /// of @p files. The first file uses `LogModel::BeginStreaming`; the
    /// rest are queued and dispatched through `AppendStreaming` from
    /// the `streamingFinished` slot.
    void StartStreamingOpenQueue(QStringList files);

    /// Pop the next file off `mPendingOpenFiles` and start parsing it.
    /// File-open errors are collected on `mPendingOpenErrors` and the
    /// summary is shown when the queue drains.
    void StreamNextPendingFile();

    void ShowParseErrors(const QString &title, const std::vector<std::string> &errors);
    void AddLogFilter(const QString &id, const loglib::LogConfiguration::LogFilter &filter);
    void UpdateFilters();
    void ApplyTableStyleSheet();

    void SetConfigurationUiEnabled(bool enabled);
    void UpdateStreamingStatus();

    /// Re-evaluate the stream toolbar's visibility against the current
    /// session mode. Called from open paths and `streamingFinished`.
    void UpdateStreamToolbarVisibility();

    /// Scroll the table to the most-recently-appended source row when
    /// Follow tail is on. Mapped through the proxy chain so the scroll
    /// lands on the correct visual row even under a sort.
    void ScrollToNewestRowIfFollowing();

    /// Re-apply the persisted retention cap to the model. Called from
    /// startup and from the preferences-Ok handler.
    void ApplyStreamingRetention();

    Ui::MainWindow *ui;
    QVBoxLayout *mLayout;
    StreamOrderProxyModel *mStreamOrderProxyModel;
    LogFilterModel *mSortFilterProxyModel;
    LogTableView *mTableView;
    LogModel *mModel;
    FindRecordWidget *mFindRecord;
    PreferencesEditor *mPreferencesEditor;
    loglib::LogConfiguration mConfiguration;
    std::unordered_map<std::string, loglib::LogConfiguration::LogFilter> mFilters;

    /// Status-bar label rendered while a streaming session is active.
    QLabel *mStatusLabel = nullptr;

    /// Toolbar holding Pause / Follow tail / Stop. Visible only during
    /// a live-tail session.
    QToolBar *mStreamToolbar = nullptr;

    /// Filename of the active stream; empty when no session is in
    /// flight.
    QString mStreamingFileName;

    /// Files queued by `StartStreamingOpenQueue` waiting to be streamed
    /// after the current file finishes.
    QStringList mPendingOpenFiles;

    /// File-open errors collected while draining `mPendingOpenFiles`.
    /// Surfaced as the post-parse summary when the queue drains.
    std::vector<std::string> mPendingOpenErrors;

    /// Streaming session kind. Gates the configuration UI, the
    /// post-parse error summary, and the live-tail-only status-bar
    /// variants. Set on open, cleared in `streamingFinished`.
    enum class SessionMode
    {
        Idle,
        Static,
        LiveTail,
    };
    SessionMode mSessionMode = SessionMode::Idle;

    [[nodiscard]] bool IsSessionActive() const noexcept { return mSessionMode != SessionMode::Idle; }
    [[nodiscard]] bool IsLiveTailSession() const noexcept { return mSessionMode == SessionMode::LiveTail; }

    /// Running line / error counts shown in the status bar.
    qsizetype mStreamingLineCount = 0;
    qsizetype mStreamingErrorCount = 0;

    /// Flips true on the first non-empty batch of a session; gates the
    /// one-shot column auto-resize so subsequent batches don't yank
    /// columns under the user's mouse.
    bool mFirstStreamingBatchSeen = false;

    /// True during the 3 s `— rotated` flash on the status bar after a
    /// rotation event (cleared by a `QTimer::singleShot`).
    bool mRotationFlashActive = false;

    /// Latched `SourceStatus::Waiting` flag. Set/cleared by
    /// `OnSourceStatusChanged`; cleared by `streamingFinished`. Drives
    /// the `Source unavailable …` status-bar variant.
    bool mSourceWaiting = false;
};
