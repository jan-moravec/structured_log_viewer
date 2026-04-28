#pragma once

#include <loglib/log_data.hpp>
#include <loglib/log_file.hpp>
#include <loglib/log_table.hpp>
#include <loglib/streaming_log_sink.hpp>

#include <QAbstractTableModel>
#include <QFuture>

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
    std::stop_token BeginStreaming(std::unique_ptr<loglib::LogFile> file);

    /// Hands the model the `QFuture` returned by the background streaming
    /// parser (typically `QtConcurrent::run`). The model retains it as a
    /// synchronisation handle so `Clear()` and the destructor can block until
    /// the worker has fully released its borrowed `LogFile*` before the mmap
    /// is unmapped — a cooperative `RequestStop()` alone is not sufficient
    /// because Stage B of the TBB pipeline runs in `parallel` mode and does
    /// not check the stop_token mid-batch. Must be called on the GUI thread
    /// immediately after kicking off the parse.
    void SetStreamingFuture(QFuture<void> future);

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

    /// Emitted from `EndStreaming`.
    void streamingFinished(bool cancelled);

private:
    loglib::LogTable mLogTable;
    QtStreamingLogSink *mSink = nullptr;

    /// Tracks the background `QtConcurrent::run` future for the active
    /// streaming parse. Owned here (not on `MainWindow`) so destructive
    /// model operations can `waitForFinished()` before tearing down the
    /// `LogTable`/`LogFile` that the worker still holds raw pointers into.
    QFutureWatcher<void> *mStreamingWatcher = nullptr;

    /// Cumulative error count for the active parse. Mirrors
    /// `mStreamingErrors.size()` so signal listeners read it cheaply.
    qsizetype mErrorCount = 0;

    /// True between `BeginStreaming()` and the matching `EndStreaming()` (or
    /// `Clear()`). Both endpoints run on the GUI thread, so this flag is
    /// race-free — unlike `mStreamingWatcher->isRunning()`, which flips to
    /// `false` as soon as the worker function returns, before the queued
    /// `OnFinished` lambda has reached the GUI thread. `Clear()` reads it to
    /// decide whether it must emit a compensating `streamingFinished(true)`
    /// signal (the queued `OnFinished` is silently dropped by the sink's
    /// generation-mismatch check, since `Clear()` calls
    /// `DropPendingBatches()` right after the join to bump the generation
    /// past whatever the worker captured during drain).
    bool mStreamingActive = false;

    std::vector<std::string> mStreamingErrors;

    static QString ConvertToSingleLineCompactQString(const std::string &string);
};
