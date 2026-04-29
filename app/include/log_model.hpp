#pragma once

#include <loglib/log_data.hpp>
#include <loglib/log_file.hpp>
#include <loglib/log_table.hpp>
#include <loglib/stop_token.hpp>
#include <loglib/streaming_log_sink.hpp>

#include <QAbstractTableModel>
#include <QFuture>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

template <typename T> class QFutureWatcher;
class QtStreamingLogSink;

enum LogModelItemDataRole
{
    UserRole = Qt::UserRole,
    SortRole,
    CopyLine
};

/// Outcome reported by `LogModel::streamingFinished`: clean completion vs.
/// external cancel vs. worker failure (the exception-escape `catch` paths
/// in `LogModel::BeginStreaming(file, parseCallable)` map to `Failed`).
enum class StreamingResult : int
{
    Success = 0,
    Cancelled = 1,
    Failed = 2,
};

class LogModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    explicit LogModel(QObject *parent = nullptr);
    ~LogModel() override;

    void AddData(loglib::LogData &&logData);
    void Clear();

    /// Resets the model and arms the bridging sink for a streaming parse
    /// against @p file (ownership transfers). Returns the parse stop_token.
    /// **Synchronous test variant** — does not start a worker thread; the
    /// caller drives `JsonParser::ParseStreaming` on the calling thread.
    /// Production code uses the two-argument overload below.
    ///
    /// Contract: pair every call with either `EndStreaming(...)` or a
    /// `Clear()` (which emits a compensating `streamingFinished` for the
    /// still-open generation). Otherwise `mStreamingActive` stays on,
    /// later `Clear()`s emit stale signals, and `MainWindow`'s
    /// configuration-UI gating never reopens. Callers that bail out
    /// between `BeginStreaming` and the parser worker must call
    /// `EndStreaming(true)` (and ideally `Sink()->RequestStop()` +
    /// `Sink()->DropPendingBatches()` for generation symmetry).
    ///
    /// Asserts the streaming watcher is idle. To replace an in-flight
    /// parse, call `Clear()` first to join the worker.
    loglib::StopToken BeginStreaming(std::unique_ptr<loglib::LogFile> file);

    /// Production overload: arms the model as above, then runs @p
    /// parseCallable on a `QtConcurrent::run` worker and parks the
    /// `QFuture` on the model so `Clear()` / `~LogModel` can
    /// `waitForFinished()` before the borrowed `LogFile*` is unmapped
    /// (TBB Stage B runs `parallel` and doesn't poll the stop_token
    /// mid-batch, so a cooperative stop alone is insufficient).
    ///
    /// @p parseCallable receives the parse `StopToken`. Throwing escapes
    /// are caught at the worker boundary and turned into a synthetic
    /// terminal `OnBatch` + `OnFinished(false)` so the GUI watchdog
    /// always observes `finished()`.
    loglib::StopToken BeginStreaming(
        std::unique_ptr<loglib::LogFile> file, std::function<void(loglib::StopToken)> parseCallable
    );

    /// Appends one streamed batch and emits the corresponding
    /// rows/columns/dataChanged signals plus the line/error counters.
    void AppendBatch(loglib::StreamedBatch batch);

    /// Finalises the streaming parse and emits `streamingFinished`.
    /// Timestamps are already promoted by Stage B / mid-stream back-fill.
    void EndStreaming(bool cancelled);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    template <typename T> std::optional<std::pair<T, T>> GetMinMaxValues(int column) const;

    const loglib::LogTable &Table() const;
    loglib::LogTable &Table();
    const loglib::LogData &Data() const;
    const loglib::LogConfiguration &Configuration() const;
    loglib::LogConfigurationManager &ConfigurationManager();

    /// GUI-side bridging sink owned by the model.
    QtStreamingLogSink *Sink();

    /// Per-line errors peeled off `StreamedBatch::errors` since the last
    /// `Clear`/`BeginStreaming`. `LogTable::AppendBatch` discards them.
    const std::vector<std::string> &StreamingErrors() const;

signals:
    /// Emitted after each batch carrying errors; cumulative count since
    /// last `Clear`/`BeginStreaming`.
    void errorCountChanged(qsizetype count);

    /// Emitted after every `AppendBatch` so the status bar still ticks
    /// on errors-only batches.
    void lineCountChanged(qsizetype count);

    /// Emitted from `EndStreaming` (and from `Clear()` as a compensating
    /// signal when the queued `OnFinished` was generation-stamped). See
    /// `StreamingResult` for the outcome encoding.
    void streamingFinished(StreamingResult result);

private:
    loglib::LogTable mLogTable;
    QtStreamingLogSink *mSink = nullptr;

    /// Background `QtConcurrent::run` future for the active parse. Owned
    /// here so destructive model operations can `waitForFinished()` before
    /// the worker's borrowed `LogTable`/`LogFile` pointers go away.
    QFutureWatcher<void> *mStreamingWatcher = nullptr;

    /// Cumulative error count for the active parse. Mirrors
    /// `mStreamingErrors.size()` so signal listeners read it cheaply.
    qsizetype mErrorCount = 0;

    /// True between `BeginStreaming()` and the matching `EndStreaming()` /
    /// `Clear()`. Both endpoints run on the GUI thread, so this flag is
    /// race-free — unlike `mStreamingWatcher->isRunning()`, which flips off
    /// as soon as the worker returns, *before* its queued `OnFinished`
    /// lambda reaches the GUI thread. `Clear()` reads this to decide
    /// whether to emit a compensating `streamingFinished` (the worker's
    /// `OnFinished` is dropped by the sink's generation-mismatch check
    /// after `DropPendingBatches()` bumps the generation post-join).
    bool mStreamingActive = false;

    std::vector<std::string> mStreamingErrors;

    static QString ConvertToSingleLineCompactQString(const std::string &string);
};

// Round-trips through `QVariant` for `QSignalSpy::value(0)` in tests and
// for cross-thread queued connections.
Q_DECLARE_METATYPE(StreamingResult)
