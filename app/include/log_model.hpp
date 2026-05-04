#pragma once

#include <loglib/bytes_producer.hpp>
#include <loglib/log_data.hpp>
#include <loglib/log_parse_sink.hpp>
#include <loglib/log_table.hpp>
#include <loglib/parser_options.hpp>
#include <loglib/stop_token.hpp>

#include <QAbstractTableModel>
#include <QFuture>

#include <cstddef>
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
    CopyLine,
    /// Source-model row index. `StreamOrderProxyModel` sorts on this
    /// (descending) for newest-first mode without disturbing the user's
    /// column sort on `LogFilterModel`.
    InsertionOrderRole,
};

/// Outcome reported by `LogModel::streamingFinished`. `Failed` is the
/// exception-escape boundary in the parser worker.
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

    /// Full teardown (`BytesProducer::Stop()` -> sink `RequestStop()` ->
    /// worker join -> paused-buffer flush -> `DropPendingBatches()`)
    /// followed by a model reset. Emits `lineCountChanged(0)` /
    /// `errorCountChanged(0)`, plus a compensating
    /// `streamingFinished(Cancelled)` if a session was still active.
    void Reset();

    /// Same teardown as `Reset()` but keeps the visible rows so the
    /// user can keep sorting/filtering/copying after Stop.
    void StopAndKeepRows();

    /// Static-file streaming entry point. Runs @p parseCallable on a
    /// `QtConcurrent::run` worker; the future is parked on the model so
    /// teardown can join before the borrowed `LogFile*` is unmapped
    /// (the TBB pipeline does not poll the stop token mid-batch).
    ///
    /// Exceptions escaping @p parseCallable are caught at the worker
    /// boundary and surfaced as a synthetic terminal
    /// `OnBatch` + `OnFinished(false)`.
    loglib::StopToken BeginStreaming(
        std::unique_ptr<loglib::FileLineSource> source, std::function<void(loglib::StopToken)> parseCallable
    );

    /// Append a follow-up file to an already-active static-file session
    /// (multi-file open). Reuses the existing `KeyIndex` so columns
    /// line up across files. Asserts the streaming watcher is idle.
    loglib::StopToken AppendStreaming(
        std::unique_ptr<loglib::FileLineSource> source, std::function<void(loglib::StopToken)> parseCallable
    );

    /// Live-tail entry point. Takes ownership of @p source, arms the
    /// sink, and spawns a `JsonParser::ParseStreaming` worker.
    ///
    /// Teardown order: `BytesProducer::Stop()` (releases I/O parked in
    /// `Read`/`WaitForBytes`) -> parser stop token -> worker join ->
    /// sink `DropPendingBatches()`. Both stop signals are needed; the
    /// parser token alone cannot unblock a worker parked on I/O.
    ///
    /// `options.stopToken` is overwritten by `Sink()->Arm()` before
    /// the worker captures it.
    loglib::StopToken BeginStreaming(std::unique_ptr<loglib::StreamLineSource> source, loglib::ParserOptions options);

    /// Test-only: install @p source, arm the sink, and return the stop
    /// token without spawning a worker. Pair every call with
    /// `EndStreaming(...)` or `Reset()`.
    loglib::StopToken BeginStreamingForSyncTest(std::unique_ptr<loglib::LineSource> source);

    /// Append one streamed batch and emit the Qt model signals. When a
    /// `RetentionCap()` is set, the visible row prefix is FIFO-evicted
    /// before insertion; over-cap batches are head-trimmed first so
    /// per-batch eviction stays O(cap).
    void AppendBatch(loglib::StreamedBatch batch);

    /// Finalise the streaming parse and emit `streamingFinished`.
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

    /// Whether a live-tail / static-streaming session is currently armed.
    /// Mirrors `Sink()->IsActive()` for callers that should not poke the
    /// sink directly.
    [[nodiscard]] bool IsStreamingActive() const noexcept;

    /// Update the in-memory line cap.
    ///   - **Running**: FIFO-trim visible rows down to the new cap.
    ///     Raising has no immediate effect.
    ///   - **Paused**: keep visible rows; trim the paused buffer so
    ///     visible+buffered stays within `cap`.
    ///   - **Idle**: just record the value for the next session.
    /// `cap == 0` means unbounded; the live-tail entry substitutes
    /// `StreamingControl::kDefaultRetentionLines` if still 0.
    void SetRetentionCap(size_t cap);

    /// Current retention cap (`0` means unbounded). GUI thread only.
    [[nodiscard]] size_t RetentionCap() const noexcept;

signals:
    /// Cumulative error count since the last `Reset` / `BeginStreaming`,
    /// emitted whenever a batch carries errors.
    void errorCountChanged(qsizetype count);

    /// Emitted after every `AppendBatch` so the status bar ticks on
    /// errors-only batches too.
    void lineCountChanged(qsizetype count);

    /// Emitted from `EndStreaming` (and from `Reset()` as a compensating
    /// signal when the queued `OnFinished` was generation-stamped).
    void streamingFinished(StreamingResult result);

    /// Rotation reported by the active `BytesProducer`. Re-emitted from
    /// the source's worker thread via a queued connection so the slot
    /// runs on the GUI thread.
    void rotationDetected();

    /// `Running`/`Waiting` transition reported by the active
    /// `BytesProducer`. Re-emitted via queued connection to the GUI.
    void sourceStatusChanged(loglib::SourceStatus status);

private:
    /// Shared `BeginStreaming` setup: install @p source into `LogTable`
    /// (or nothing for the sync-test entry), reset the model, and arm
    /// the sink. Reserves per-line offsets up front for `FileLineSource`
    /// inputs; no-op for stream sources.
    void BeginStreamingShared(std::unique_ptr<loglib::LineSource> source);

    /// Shared implementation of `Reset()` / `StopAndKeepRows()`. Runs
    /// the full teardown, then resets `mLogTable` if @p resetTable is
    /// true. Order: teardown → reset → `lineCountChanged(0)` →
    /// compensating `streamingFinished`.
    void TeardownStreamingSessionInternal(bool resetTable);

    loglib::LogTable mLogTable;
    QtStreamingLogSink *mSink = nullptr;

    /// Future for the active parse worker; destructive ops join it
    /// before the borrowed `LogTable` / `LineSource` are torn down.
    QFutureWatcher<void> *mStreamingWatcher = nullptr;

    /// Live byte producer for the active session, or nullptr when none.
    /// Teardown calls `Stop()` on it before joining the worker.
    [[nodiscard]] loglib::BytesProducer *ActiveProducer() noexcept;

    qsizetype mErrorCount = 0;

    /// Race-free "still streaming" flag.
    /// `mStreamingWatcher->isRunning()` flips off before the queued
    /// `OnFinished` reaches the GUI; this flag stays set until
    /// `EndStreaming`/`Reset` runs on the GUI thread.
    bool mStreamingActive = false;

    std::vector<std::string> mStreamingErrors;

    /// Retention cap. `0` means unbounded; the live-tail entry applies
    /// `StreamingControl::kDefaultRetentionLines` when still 0 at
    /// session start.
    size_t mRetentionCap = 0;

    static QString ConvertToSingleLineCompactQString(const std::string &string);
};

Q_DECLARE_METATYPE(StreamingResult)
Q_DECLARE_METATYPE(loglib::SourceStatus)
