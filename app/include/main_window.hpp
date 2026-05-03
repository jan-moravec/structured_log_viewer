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

    /// **Single sync point** for the newest-first display orientation.
    /// Reads `StreamingControl::IsNewestFirst()` once and propagates
    /// the boolean to every GUI piece that needs to track it in
    /// lockstep:
    ///   1. `StreamOrderProxyModel::SetReversed` — flips the proxy's
    ///      `InsertionOrderRole` sort direction.
    ///   2. `LogTableView::SetTailEdge` — picks Top vs Bottom for the
    ///      Follow-tail anchor + user-scroll-detection heuristic.
    ///   3. `setAlternatingRowColors` — disabled in newest-first mode
    ///      because top-insertion shifts every visible row's parity on
    ///      every batch (see body for the gritty details).
    /// Tests drive this directly to bypass the preferences-editor
    /// round-trip; production calls it from startup and from
    /// `PreferencesEditor::streamingDisplayOrderChanged`. Idempotent.
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

    /// Toggle pause / resume on the bridging sink. Slot bound to
    /// `actionPauseStream` (toolbar / Stream menu).
    void TogglePauseStream(bool paused);

    /// Stop the active stream. Slot bound to `actionStopStream`.
    void StopStream();

    /// Source-thread rotation event re-emitted on the GUI thread by
    /// `LogModel::rotationDetected`; flashes the brief `— rotated`
    /// suffix in the status bar.
    void OnRotationDetected();

    /// Source-thread status transition re-emitted on the GUI thread by
    /// `LogModel::sourceStatusChanged`.
    /// Latches `mSourceWaiting` and refreshes the status-bar label so it
    /// shows `Source unavailable …` while the source is in
    /// `SourceStatus::Waiting`.
    void OnSourceStatusChanged(loglib::SourceStatus status);

private:
    /// Try to load @p file as a `LogConfiguration`; returns true and
    /// fires `UpdateUi` on success. Used by the single-file open and
    /// drag-drop paths to preserve the historical "drop a config file
    /// to load it" affordance.
    bool TryLoadAsConfiguration(const QString &file);

    /// Reset the model + filter state and start a sequential streaming
    /// open of @p files. The first file goes through
    /// `LogModel::BeginStreaming`; the rest are queued on
    /// `mPendingOpenFiles` and dispatched through
    /// `LogModel::AppendStreaming` from the `streamingFinished` slot
    /// once the previous file's parse finishes. Single-file opens use
    /// the same path -- streaming is the only static-file populate
    /// route after the Phase 6 cleanup.
    void StartStreamingOpenQueue(QStringList files);

    /// Pop one file off `mPendingOpenFiles` and start parsing it. The
    /// first file in a session uses `BeginStreaming`; subsequent files
    /// use `AppendStreaming` so the existing rows + `KeyIndex` survive
    /// across files. File-open errors are collected on
    /// `mPendingOpenErrors` and the next file is attempted; the
    /// summary is shown when the queue drains.
    void StreamNextPendingFile();

    void ShowParseErrors(const QString &title, const std::vector<std::string> &errors);
    void AddLogFilter(const QString &id, const loglib::LogConfiguration::LogFilter &filter);
    void UpdateFilters();
    void ApplyTableStyleSheet();

    void SetConfigurationUiEnabled(bool enabled);
    void UpdateStreamingStatus();

    /// Re-evaluate the visibility of the stream toolbar against
    /// `mModel->IsStreamingActive()` (4.10 task 4.12). Called from
    /// `BeginStreaming` and `streamingFinished`.
    void UpdateStreamToolbarVisibility();

    /// Scroll the table to the most-recently-appended source row when
    /// `actionFollowTail->isChecked()`. Mapped through
    /// the proxy model so it lands on the correct visual row even under
    /// a sort.
    void ScrollToNewestRowIfFollowing();

    /// Re-apply the persisted `streaming/retentionLines` value to
    /// `mModel->SetRetentionCap`. Called from
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

    /// Status-bar label that shows "Parsing <file> — N lines, M errors"
    /// while a streaming parse is in flight.
    QLabel *mStatusLabel = nullptr;

    /// Toolbar holding Pause / Follow newest / Stop. Visible only while
    /// `mModel->IsStreamingActive() == true`.
    QToolBar *mStreamToolbar = nullptr;

    /// Display name of the file currently being streamed; used to render
    /// `mStatusLabel`. Empty when no parse is in flight.
    QString mStreamingFileName;

    /// Files queued by `StartStreamingOpenQueue` and not yet streamed.
    /// `streamingFinished` pops the next one and calls
    /// `LogModel::AppendStreaming` until empty. Cleared on cancel /
    /// failure / `Clear` so a new open does not inherit a stale queue.
    QStringList mPendingOpenFiles;

    /// File-open errors accumulated while draining `mPendingOpenFiles`.
    /// Any file that fails to open is skipped (the queue continues with
    /// the next entry); the summary is shown to the user when the
    /// queue drains.
    std::vector<std::string> mPendingOpenErrors;

    /// What kind of streaming session, if any, the GUI is currently
    /// driving. Set on entry to `BeginStreaming`/`AppendStreaming` and
    /// reset in the `streamingFinished` slot.
    ///
    /// The value gates the configuration UI, the post-parse error
    /// summary, and the live-tail-only status-bar variants. The previous
    /// `mStreamingActive` / `mLiveTailActive` bool pair encoded the
    /// same three states (`Idle`, `Static`, `LiveTail`) but allowed an
    /// impossible fourth one (`!streaming && liveTail`); the enum
    /// makes the state space explicit.
    enum class SessionMode
    {
        Idle,
        Static,
        LiveTail,
    };
    SessionMode mSessionMode = SessionMode::Idle;

    [[nodiscard]] bool IsSessionActive() const noexcept { return mSessionMode != SessionMode::Idle; }
    [[nodiscard]] bool IsLiveTailSession() const noexcept { return mSessionMode == SessionMode::LiveTail; }

    /// Running line / error count snapshot for the status-bar label.
    qsizetype mStreamingLineCount = 0;
    qsizetype mStreamingErrorCount = 0;

    /// Tracks whether the first non-empty streaming batch has landed yet.
    /// Reset on `BeginStreaming`, flipped to true on the first
    /// `lineCountChanged` with a non-zero count. While `false` and
    /// live-tail is active, `UpdateUi()` runs after the batch lands;
    /// thereafter, never.
    bool mFirstStreamingBatchSeen = false;

    /// True for the duration of the brief 3 s `— rotated` flash on the
    /// status bar after a rotation event.
    /// A `QTimer::singleShot` clears it.
    bool mRotationFlashActive = false;

    /// Latched `SourceStatus::Waiting` flag. Set by `OnSourceStatusChanged(Waiting)`, cleared by
    /// `OnSourceStatusChanged(Running)` and by `streamingFinished`
    /// (either terminal outcome — the source is no longer "waiting" if
    /// the whole session tore down). Used by `UpdateStreamingStatus`
    /// to render the `Source unavailable …` label variant.
    bool mSourceWaiting = false;
};
