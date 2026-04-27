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
#include <QFuture>
#include <QFutureWatcher>
#include <QLabel>
#include <QMainWindow>
#include <QMimeData>
#include <QPushButton>
#include <QString>
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

private:
    void OpenFileInternal(const QString &file, std::vector<std::string> &errors);
    void OpenFilesWithParser(const QString &dialogTitle, std::unique_ptr<loglib::LogParser> parser);
    void ShowParseErrors(const QString &title, const std::vector<std::string> &errors);
    void AddLogFilter(const QString &id, const loglib::LogConfiguration::LogFilter &filter);
    void UpdateFilters();
    void ApplyTableStyleSheet();

    /**
     * @brief Streams a JSON log through `LogParser::ParseStreaming` on a
     *        background thread.
     *
     * Locks the configuration UI for the parse, hands the model's
     * `QtStreamingLogSink` and a freshly-installed `stop_token` to the
     * parser, and tracks the parse via a `QFutureWatcher` so a second
     * `OpenJsonStreaming` call doesn't leave the previous one dangling.
     *
     * @return true if the streaming pipeline started; false if the file
     *              could not be opened (in which case @p errors carries the
     *              reason and the caller falls back to the synchronous path).
     */
    bool OpenJsonStreaming(const QString &file, std::vector<std::string> &errors);
    void SetConfigurationUiEnabled(bool enabled);
    void UpdateStreamingStatus();

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

    /// Display name of the file currently being streamed; used to render
    /// `mStatusLabel`. Empty when no parse is in flight.
    QString mStreamingFileName;

    /// Tracks the background `QtConcurrent::run` future for the active
    /// streaming parse. Reused so a second open can detect (and wait on /
    /// cancel) a previous one without leaking. `QFutureWatcher` is parented
    /// to the window so its lifetime is bounded automatically.
    QFutureWatcher<void> *mStreamingWatcher = nullptr;

    /// True between `BeginStreaming` and the matching `streamingFinished`
    /// signal. Used to gate the configuration UI and to suppress the
    /// post-parse error summary on cancellation.
    bool mStreamingActive = false;

    /// Errors accumulated from the streaming sink during the active parse.
    /// Surfaced via the existing `QMessageBox` summary on
    /// `streamingFinished(false)`.
    std::vector<std::string> mStreamingErrors;

    /// Running line / error count snapshot for the status-bar label.
    qsizetype mStreamingLineCount = 0;
    qsizetype mStreamingErrorCount = 0;
};
