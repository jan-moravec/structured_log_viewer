#pragma once

#include "find_record_widget.hpp"
#include "log_filter_model.hpp"
#include "log_model.hpp"
#include "log_table_view.hpp"
#include "preferences_editor.hpp"

#include <loglib/log_configuration.hpp>
#include <loglib/log_parser.hpp>

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QLabel>
#include <QMainWindow>
#include <QMimeData>
#include <QPushButton>
#include <QString>
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

protected:
    bool event(QEvent *event) override;

private slots:
    void OpenFiles();
    void OpenJsonLogs();
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

    /// Toggle pause / resume on the bridging sink (PRD 4.2). Slot bound to
    /// `actionPauseStream` (toolbar / Stream menu).
    void TogglePauseStream(bool paused);

    /// Stop the active stream (PRD 4.7). Slot bound to `actionStopStream`.
    void StopStream();

    /// Source-thread rotation event re-emitted on the GUI thread by
    /// `LogModel::rotationDetected`; flashes the brief `— rotated`
    /// suffix in the status bar (PRD 4.8.7.v / 5.8).
    void OnRotationDetected();

private:
    void OpenFileInternal(const QString &file, std::vector<std::string> &errors);
    void OpenFilesWithParser(const QString &dialogTitle, std::unique_ptr<loglib::LogParser> parser);
    void ShowParseErrors(const QString &title, const std::vector<std::string> &errors);
    void AddLogFilter(const QString &id, const loglib::LogConfiguration::LogFilter &filter);
    void UpdateFilters();
    void ApplyTableStyleSheet();

    /// Streams a JSON log through `LogParser::ParseStreaming` on a background
    /// thread, locking the configuration UI for the parse.
    /// @return true if streaming started; false if @p file could not be
    /// opened (in which case @p errors carries the reason and the caller
    /// falls back to the synchronous path).
    bool OpenJsonStreaming(const QString &file, std::vector<std::string> &errors);
    void SetConfigurationUiEnabled(bool enabled);
    void UpdateStreamingStatus();

    /// Re-evaluate the visibility of the stream toolbar against
    /// `mModel->IsStreamingActive()` (4.10 task 4.12). Called from
    /// `BeginStreaming` and `streamingFinished`.
    void UpdateStreamToolbarVisibility();

    /// Scroll the table to the most-recently-appended source row when
    /// `actionFollowTail->isChecked()` (PRD 4.3 / task 5.7). Mapped through
    /// the proxy model so it lands on the correct visual row even under
    /// a sort.
    void ScrollToNewestRowIfFollowing();

    /// Re-apply the persisted `streaming/retentionLines` value to
    /// `mModel->SetRetentionCap` (PRD 4.5.5 / task 5.12). Called from
    /// startup and from the preferences-Ok handler.
    void ApplyStreamingRetention();

    Ui::MainWindow *ui;
    QVBoxLayout *mLayout;
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

    /// Toolbar holding Pause / Follow tail / Stop. Visible only while
    /// `mModel->IsStreamingActive() == true` (PRD §6 *Toolbar*; task 5.3).
    QToolBar *mStreamToolbar = nullptr;

    /// Display name of the file currently being streamed; used to render
    /// `mStatusLabel`. Empty when no parse is in flight.
    QString mStreamingFileName;

    /// True between `BeginStreaming` and the matching `streamingFinished`
    /// signal. Used to gate the configuration UI and to suppress the
    /// post-parse error summary on cancellation.
    bool mStreamingActive = false;

    /// True while the active session was opened via **File → Open Log
    /// Stream…** (`TailingFileSource`); false for the static-streaming
    /// path. Drives the status-bar label (PRD §6 *Status bar*).
    bool mLiveTailActive = false;

    /// Running line / error count snapshot for the status-bar label.
    qsizetype mStreamingLineCount = 0;
    qsizetype mStreamingErrorCount = 0;

    /// Tracks whether the first non-empty streaming batch has landed yet
    /// (PRD §6 *Column auto-resize during streaming*; task 5.9). Reset on
    /// `BeginStreaming`, flipped to true on the first `lineCountChanged`
    /// with a non-zero count. While `false` and live-tail is active,
    /// `UpdateUi()` runs after the batch lands; thereafter, never.
    bool mFirstStreamingBatchSeen = false;

    /// True for the duration of the brief 3 s `— rotated` flash on the
    /// status bar after a rotation event (PRD §6 *Rotation indicator*).
    /// A `QTimer::singleShot` clears it.
    bool mRotationFlashActive = false;
};
