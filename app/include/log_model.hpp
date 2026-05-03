#pragma once

#include <loglib/log_data.hpp>
#include <loglib/bytes_producer.hpp>
#include <loglib/log_table.hpp>
#include <loglib/parser_options.hpp>
#include <loglib/stop_token.hpp>
#include <loglib/streaming_log_sink.hpp>

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
    /// Returns the source-model row index (`index.row()`) regardless of
    /// column. Used by the `StreamOrderProxyModel` to reverse the visual
    /// order of streamed lines (newest-first mode) without touching the
    /// user-clicked column sort that lives on the outer
    /// `LogFilterModel`. Comparing this role with `Qt::DescendingOrder`
    /// places the most-recently-appended row at proxy row 0.
    InsertionOrderRole,
};

/// Outcome reported by `LogModel::streamingFinished`: clean completion vs.
/// external cancel vs. worker failure (the exception-escape `catch` paths
/// in `LogModel::BeginStreaming(FileLineSource, parseCallable)` map to
/// `Failed`).
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
    /// Default retention cap used by `BeginStreaming(StreamLineSource, ...)`
    /// when no other value has been set. Mirrors PRD 4.5.2 (`N = 10 000`,
    /// allowed range 1 000 .. 1 000 000).
    static constexpr size_t kDefaultRetentionLines = 10'000;

    explicit LogModel(QObject *parent = nullptr);
    ~LogModel() override;

    void AddData(loglib::LogData &&logData);

    /// Tears the full model down: runs the PRD 4.7.2.i streaming
    /// teardown order (source `Stop()` → sink `RequestStop()` → worker
    /// `waitForFinished()` → paused-buffer flush → `DropPendingBatches()`)
    /// **and** then resets `mLogTable`, emitting `lineCountChanged(0)` /
    /// `errorCountChanged(0)`. Called from every "open a new session"
    /// entry point. The compensating `streamingFinished(Cancelled)` is
    /// emitted when this call observed `mStreamingActive == true`.
    void Clear();

    /// Ends the active streaming session but **preserves the visible
    /// rows** (PRD 4.7.1). Runs the same teardown sequence as `Clear()`
    /// — including the paused-buffer flush so already-parsed rows land
    /// in the model before teardown — but does *not* reset `mLogTable`.
    /// Used by `MainWindow::StopStream` so the user can keep sorting,
    /// filtering, searching, and copying rows after Stop. Emits the
    /// compensating `streamingFinished(Cancelled)` when this call
    /// observed `mStreamingActive == true`.
    void Detach();

    /// Resets the model and arms the bridging sink for a streaming parse
    /// against @p source (ownership transfers); returns the parse
    /// stop_token. **Synchronous test variant** — does not start a worker
    /// thread.
    ///
    /// Pair every call with `EndStreaming(...)` or `Clear()`; otherwise
    /// `mStreamingActive` stays on and the GUI's configuration gate never
    /// reopens. Asserts the streaming watcher is idle on entry.
    loglib::StopToken BeginStreaming(std::unique_ptr<loglib::FileLineSource> source);

    /// Production overload for the static-file path: arms the model and
    /// runs @p parseCallable on a `QtConcurrent::run` worker, parking the
    /// `QFuture` on the model so `Clear()` / `~LogModel` can
    /// `waitForFinished()` before the borrowed `LogFile*` is unmapped
    /// (cooperative stop alone is insufficient — TBB Stage B runs in
    /// parallel without polling the token mid-batch).
    ///
    /// @p parseCallable receives the parse `StopToken`. Exceptions escaping
    /// it are caught at the worker boundary and converted into a synthetic
    /// terminal `OnBatch` + `OnFinished(false)`.
    loglib::StopToken BeginStreaming(
        std::unique_ptr<loglib::FileLineSource> source, std::function<void(loglib::StopToken)> parseCallable
    );

    /// Live-tail entry point. Takes ownership of @p source for the session
    /// lifetime, arms the sink, and spawns a `QtConcurrent::run` worker
    /// that drives `JsonParser::ParseStreaming(*source, *sink, options)`.
    /// Behavior on stop / model teardown is the mandatory PRD 4.7.2.i
    /// order: `BytesProducer::Stop()` (on the producer owned by the
    /// `StreamLineSource`) → parser `stopToken.request_stop()` →
    /// `mStreamingWatcher->waitForFinished()` → sink
    /// `DropPendingBatches()`. The two stop signals are deliberately
    /// distinct because the parser's stop token alone does not unblock a
    /// worker parked in `Read` / `WaitForBytes`.
    ///
    /// `options.stopToken` is overwritten by `Sink()->Arm()` before the
    /// worker captures it. `options.configuration` (if non-null) is
    /// snapshotted by the parser's hot loop.
    ///
    /// The `StreamLineSource` is installed into `LogTable` and outlives
    /// every `LogLine` it owns; the worker emits `LogLine`s tagged with
    /// that source so each row stays resolvable via `LineSource::RawLine`
    /// after parsing has moved on (PRD 4.9.7.ii / 4.10.4).
    loglib::StopToken BeginStreaming(
        std::unique_ptr<loglib::StreamLineSource> source, loglib::ParserOptions options
    );

    /// **Synchronous test variant** for the live-tail path. Installs
    /// @p source into the table and arms the sink; does not start a
    /// worker thread. Pair every call with `EndStreaming(...)` or
    /// `Clear()`. Mirrors the static-file `BeginStreaming(unique_ptr<
    /// FileLineSource>)` overload above; callers that drive synthetic
    /// `LogLine` batches via `AppendBatch` use this to install a
    /// long-lived source the synthetic lines can reference.
    loglib::StopToken BeginStreaming(std::unique_ptr<loglib::StreamLineSource> source);

    /// Appends one streamed batch and emits the corresponding
    /// rows/columns/dataChanged signals plus the line/error counters.
    /// FIFO eviction (PRD 4.5 / 4.10.3) runs before insertion when the
    /// configured retention cap is set: the prefix of the model is
    /// dropped via `beginRemoveRows` / `endRemoveRows` so the visible
    /// rows + appended rows fit. Giant batches
    /// (`batch.lines.size() > retentionCap`) collapse the head of the
    /// batch directly, keeping per-batch eviction O(cap) rather than
    /// O(batch).
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

    /// Whether a live-tail / static-streaming session is currently armed.
    /// Mirrors `Sink()->IsActive()` for callers that should not poke the
    /// sink directly (PRD 4.10 task 4.12).
    [[nodiscard]] bool IsStreamingActive() const noexcept;

    /// Update the in-memory line cap (PRD 4.5.5):
    ///   - **Running**: if the new cap is below the visible row count,
    ///     immediately FIFO-trim the existing rows. Raising has no
    ///     immediate effect.
    ///   - **Paused**: record the new cap, leave the visible rows alone,
    ///     and trim the paused buffer to `cap - visible` so the
    ///     visible+buffered total stays within `cap` (4.5.5.ii).
    ///   - **Idle**: just record the value for the next `BeginStreaming`.
    /// `cap == 0` is treated as "unbounded" (back-compat with the
    /// static-file path; the live-tail entry point applies
    /// `kDefaultRetentionLines` if no value has been set).
    void SetRetentionCap(size_t cap);

    /// Current retention cap (`0` means unbounded). GUI thread only.
    [[nodiscard]] size_t RetentionCap() const noexcept;

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

    /// Emitted on the GUI thread when the active `BytesProducer` reports a
    /// rotation event (PRD 4.8.7.v). The `MainWindow` consumes this to
    /// flash the brief `— rotated` suffix in the status bar (5.8). The
    /// callback fires from the source's worker thread; the model
    /// re-emits via a queued connection so the slot runs on the GUI
    /// thread.
    void rotationDetected();

    /// Emitted on the GUI thread when the active `BytesProducer` transitions
    /// between `Running` and `Waiting` (PRD 4.8.8 / §6 *Status bar*).
    /// The callback fires from the source's worker thread; the model
    /// re-emits via a queued connection so the slot runs on the GUI
    /// thread. `MainWindow::OnSourceStatusChanged` toggles the
    /// `Source unavailable` status-bar variant.
    void sourceStatusChanged(loglib::SourceStatus status);

private:
    /// Static-file variant of the shared `BeginStreaming` setup: installs
    /// @p source (or no source, for the synthetic-batch test path) into
    /// `LogTable`, resets the model and arms the sink. The parser worker
    /// emits `LogLine`s tagged with @p source so the model layer can
    /// resolve each row's raw bytes via `LineSource::RawLine` after
    /// parsing has moved on (PRD 4.9.4).
    void BeginStreamingShared(std::unique_ptr<loglib::FileLineSource> source);

    /// Live-tail variant of `BeginStreamingShared`: hands a long-lived
    /// `StreamLineSource` to `LogTable`. The parser worker emits
    /// `LogLine`s tagged with @p source so the model layer can resolve
    /// each row's raw bytes via `LineSource::RawLine` after parsing has
    /// moved on (PRD 4.10.4).
    void BeginStreamingShared(std::unique_ptr<loglib::StreamLineSource> source);

    /// Shared implementation of `Clear()` / `Detach()`. Runs the full
    /// PRD 4.7.2.i teardown, then — if @p resetTable is true — does the
    /// `beginResetModel` / `mLogTable.Reset` / zero-count emit sequence.
    /// The compensating `streamingFinished(Cancelled)` is emitted last,
    /// preserving the existing test-observed ordering (teardown → reset
    /// → `lineCountChanged(0)` → `streamingFinished`).
    void TeardownStreamingSessionInternal(bool resetTable);

    loglib::LogTable mLogTable;
    QtStreamingLogSink *mSink = nullptr;

    /// Future for the active parse worker; destructive ops join it before
    /// the borrowed `LogTable`/`LineSource` references are torn down.
    QFutureWatcher<void> *mStreamingWatcher = nullptr;

    /// Returns the live byte producer for the active streaming session
    /// (`LogTable::Data().FrontStreamSource()->Producer()` for live-tail
    /// sessions). Returns nullptr for static-file sessions and when no
    /// session is active. Used by the PRD 4.7.2.i teardown to call
    /// `Stop()` before joining the worker.
    [[nodiscard]] loglib::BytesProducer *ActiveProducer() noexcept;

    /// Cumulative error count for the active parse (mirrors `mStreamingErrors.size()`).
    qsizetype mErrorCount = 0;

    /// GUI-thread-only flag set by `BeginStreaming()`, cleared by
    /// `EndStreaming()` / `Clear()`. Race-free unlike
    /// `mStreamingWatcher->isRunning()`, which flips off before the queued
    /// `OnFinished` lambda reaches the GUI thread. `Clear()` consults it to
    /// decide whether to emit a compensating `streamingFinished`.
    bool mStreamingActive = false;

    std::vector<std::string> mStreamingErrors;

    /// Configurable retention cap (PRD 4.5). `0` means unbounded
    /// (static-path / sync-test-variant default). Live-tail
    /// `BeginStreaming(StreamLineSource, ...)` applies
    /// `kDefaultRetentionLines` when this is still 0 at session start.
    /// `SetRetentionCap` updates this and applies the in-flight trim
    /// rules.
    size_t mRetentionCap = 0;

    static QString ConvertToSingleLineCompactQString(const std::string &string);
};

Q_DECLARE_METATYPE(StreamingResult)
Q_DECLARE_METATYPE(loglib::SourceStatus)
