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
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

template <typename T> class QFutureWatcher;
class QtStreamingLogSink;

enum LogModelItemDataRole
{
    UserRole = Qt::UserRole,
    SortRole,
    CopyLine,
    /// Source-model row index, used as a stable sort tie-break.
    InsertionOrderRole,
    /// `loglib::EnumValueId` as `qint32` for `DictRef` slots; invalid
    /// otherwise. Kept for external readers; the filter proxy bypasses
    /// it and queries `LogTable` directly.
    EnumValueRole,
};

/// Outcome reported by `LogModel::streamingFinished`.
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
    /// Test-only overload with a custom bounded-queue capacity for the
    /// embedded `QtStreamingLogSink`.
    LogModel(QObject *parent, std::size_t pendingCapacity);
    ~LogModel() override;

    /// Full teardown followed by a model reset. Emits `lineCountChanged(0)`,
    /// `errorCountChanged(0)`, and a compensating `streamingFinished` if
    /// a session was still active.
    void Reset();

    /// Same teardown as `Reset()` but keeps visible rows for post-stop
    /// sort/filter/copy.
    void StopAndKeepRows();

    /// Static-file streaming entry point. Runs @p parseCallable on a
    /// `QtConcurrent::run` worker; the future is parked on the model so
    /// teardown can join before the borrowed `LogFile*` is unmapped.
    /// Exceptions escaping the callable surface as a synthetic terminal
    /// `OnBatch` + `OnFinished(false)`.
    loglib::StopToken BeginStreaming(
        std::unique_ptr<loglib::FileLineSource> source, std::function<void(loglib::StopToken)> parseCallable
    );

    /// Append a follow-up file to an already-active static-file session.
    /// Reuses the existing `KeyIndex` so columns line up across files.
    loglib::StopToken AppendStreaming(
        std::unique_ptr<loglib::FileLineSource> source, std::function<void(loglib::StopToken)> parseCallable
    );

    /// Live-tail entry point. Takes ownership of @p source, arms the sink,
    /// and spawns a `JsonParser::ParseStreaming` worker. Teardown order:
    /// producer Stop → parser stop token → worker join → sink drain. Both
    /// stop signals are required; the parser token alone cannot unblock a
    /// worker parked on I/O. `options.stopToken` is overwritten with the
    /// sink's token before the worker captures it.
    loglib::StopToken BeginStreaming(std::unique_ptr<loglib::StreamLineSource> source, loglib::ParserOptions options);

    /// Test-only: install @p source and arm the sink without spawning a
    /// worker. Pair with `EndStreaming(...)` or `Reset()`.
    loglib::StopToken BeginStreamingForSyncTest(std::unique_ptr<loglib::LineSource> source);

    /// Append one streamed batch and emit Qt model signals. When a
    /// `RetentionCap()` is set, FIFO-evict the visible prefix before
    /// insertion; over-cap batches are head-trimmed so per-batch
    /// eviction stays O(cap).
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

    /// Per-line errors collected since the last `Reset`/`BeginStreaming`.
    /// Also appends a synthetic message for any newly-dropped batches
    /// reported by the sink (back-pressure shutdown).
    const std::vector<std::string> &StreamingErrors() const;

    /// Whether a streaming session is currently armed.
    [[nodiscard]] bool IsStreamingActive() const noexcept;

    /// Update the in-memory line cap.
    ///   - Running: FIFO-trim visible rows to the new cap; raising the
    ///     cap has no immediate effect.
    ///   - Paused: trim the paused buffer so visible+buffered <= cap.
    ///   - Idle: record for the next session.
    /// `cap == 0` means unbounded; live-tail substitutes
    /// `StreamingControl::DEFAULT_RETENTION_LINES` if still 0.
    void SetRetentionCap(size_t cap);

    /// Current retention cap (`0` means unbounded). GUI thread only.
    [[nodiscard]] size_t RetentionCap() const noexcept;

    /// UTF-8 bytes -> single-line, simplified `QString` (the
    /// `Qt::DisplayRole` representation). Public so the filter proxy's
    /// `MatchRow` and `MainWindow::MakeStringMatcher` can apply the
    /// same normalisation the user sees on screen.
    static QString ConvertToSingleLineCompactQString(std::string_view bytes);

#ifdef LOGAPP_BUILD_TESTING
    /// Test-only: move a column with `beginMoveColumns`/`endMoveColumns`
    /// so `columnsMoved` propagates through the proxy chain.
    /// Returns `true` on success.
    bool MoveColumnForTest(int srcIndex, int destIndex);
#endif

signals:
    /// Cumulative error count, emitted when a batch carries errors.
    void errorCountChanged(qsizetype count);

    /// Emitted after every `AppendBatch`.
    void lineCountChanged(qsizetype count);

    /// Emitted from `EndStreaming` (and as a compensating signal from
    /// `Reset()` when the queued `OnFinished` was generation-stamped).
    void streamingFinished(StreamingResult result);

    /// Rotation reported by the active producer; re-emitted on the GUI.
    void rotationDetected();

    /// Producer status transition; re-emitted on the GUI.
    void sourceStatusChanged(loglib::SourceStatus status);

    /// Emitted when the set of `Type::Enumeration` columns or any of
    /// their dictionaries changes shape (auto-promotion, dict growth,
    /// or end-of-stream finalisation). `MainWindow` rebuilds active
    /// enum filter rules on every emit so newly-interned ids stay on
    /// the bitset fast path.
    void enumColumnsChanged();

private:
    /// Shared `BeginStreaming` setup: install @p source, reset the
    /// model, and arm the sink. Reserves per-line offsets for
    /// `FileLineSource` inputs.
    void BeginStreamingShared(std::unique_ptr<loglib::LineSource> source);

    /// Shared implementation of `Reset()` / `StopAndKeepRows()`.
    void TeardownStreamingSessionInternal(bool resetTable);

    loglib::LogTable mLogTable;
    QtStreamingLogSink *mSink = nullptr;

    /// Future for the active parse worker.
    QFutureWatcher<void> *mStreamingWatcher = nullptr;

    /// Producer of the active session, or nullptr.
    [[nodiscard]] loglib::BytesProducer *ActiveProducer() noexcept;

    qsizetype mErrorCount = 0;

    /// Race-free "still streaming" flag; the watcher's `isRunning()`
    /// flips off before the queued `OnFinished` reaches the GUI.
    bool mStreamingActive = false;

    mutable std::vector<std::string> mStreamingErrors;

    /// High-water mark of sink-dropped batches we've already surfaced.
    mutable std::size_t mLastReportedShutdownDropCount = 0;

    /// Retention cap; `0` means unbounded.
    size_t mRetentionCap = 0;
};

Q_DECLARE_METATYPE(StreamingResult)
Q_DECLARE_METATYPE(loglib::SourceStatus)
