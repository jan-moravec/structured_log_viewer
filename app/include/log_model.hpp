#pragma once

#include <loglib/bytes_producer.hpp>
#include <loglib/log_data.hpp>
#include <loglib/log_level.hpp>
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
#include <unordered_map>
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

/// Shape change reported by `LogModel::enumColumnsChanged`, so receivers
/// can scope their reaction:
///   - `Promoted` -- a column flipped into `Type::Enumeration`. The
///     proxy's `EnumDictRank` cache has no entry yet, so no
///     invalidation is needed; filter rules built before the promotion
///     want a rebuild onto the bitset fast path.
///   - `Grew` -- an existing enum column's dictionary gained at least
///     one id. `EnumRankFor` self-heals via its `DictSize()` check,
///     so the rank cache stays valid; filter rules only need a rebuild
///     if a previously-unresolved selected value just got interned.
///   - `Demoted` -- an enum column lost its dictionary (registry
///     erase). The cached `EnumDictionary*` is now dangling, so the
///     rank cache entry must be dropped and rules rebuilt onto the
///     string-set fallback.
enum class EnumColumnsChangeReason : int
{
    Promoted = 0,
    Grew = 1,
    Demoted = 2,
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
    /// `Qt::DisplayRole` representation). Public so `MatchRow` and
    /// `MainWindow::MakeStringMatcher` apply the same normalisation
    /// the user sees on screen.
    static QString ConvertToSingleLineCompactQString(std::string_view bytes);

    /// True iff @p bytes is already byte-equal to
    /// `ConvertToSingleLineCompactQString(bytes).toUtf8()`: pure 7-bit
    /// ASCII, no leading/trailing space, no double-space, no
    /// `\n`/`\r`/`\t`/`\v`/`\f`/control byte. When both pattern and
    /// haystack pass, `MakeStringMatcher`'s `Exactly` / `Contains`
    /// paths byte-compare directly and skip the
    /// `QString::fromUtf8` + `simplified()` walk. Early-exits on the
    /// first violating byte so the common ASCII log line costs a
    /// single linear scan.
    [[nodiscard]] static bool IsSingleLineAsciiTrim(std::string_view bytes) noexcept;

    /// Move column @p srcIndex to @p destIndex (absolute final
    /// position, not Qt's "insert before"). Emits `columnsMoved`
    /// and remaps `LogFilter::row` so saved filters follow the
    /// column. Returns `true` on success.
    bool MoveColumn(int srcIndex, int destIndex);

    /// Tell the view to refresh its column structure after an
    /// out-of-band rewrite of the configuration (e.g. `Load`).
    /// Wraps a `beginResetModel` / `endResetModel`; row data is
    /// untouched.
    void NotifyConfigurationReplaced();

    /// Canonical-level -> raw-dictionary-bytes mapping captured at the
    /// most recent `AppendBatch` for any `Type::Level` column that
    /// demoted to `Type::String` during that batch. Returns `nullptr`
    /// when no such demote happened in the last batch for the given
    /// @p columnIndex. The mapping reflects the dictionary contents
    /// the moment before the demote, so callers consuming the
    /// adjacent `enumColumnsChanged(Demoted)` signal can translate
    /// canonical-name filters (e.g. `"Info"`) back into the raw
    /// dictionary entries (`"info"`, `"INF"`, `"30"`, ...) that were
    /// observed for that column. Cleared at the start of every
    /// subsequent `AppendBatch`.
    [[nodiscard]] const std::unordered_map<loglib::LogLevel, std::vector<std::string>> *
    LastBatchLevelDemoteMappingFor(int columnIndex) const noexcept;

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

    /// Emitted when a `Type::Enumeration` column or its dictionary
    /// changes shape (promotion, dict growth, end-of-stream finalise).
    /// One signal per affected column so receivers can scope by
    /// @p columnIndex (source-table coords). `columnIndex == -1` is
    /// "unscoped" (registry-wide sweep) -- treat as "any enum filter
    /// may need attention". See `EnumColumnsChangeReason`.
    void enumColumnsChanged(EnumColumnsChangeReason reason, int columnIndex);

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

    /// Per-batch capture: column -> (canonical level -> raw dictionary
    /// bytes). Populated when a `Type::Level` column loses its
    /// dictionary during the current `AppendBatch`. Consumed by
    /// `MainWindow::enumColumnsChanged(Demoted)` to translate stored
    /// canonical-name filter selections into the raw dictionary
    /// entries that resolved to those levels at demote time. Cleared
    /// at the top of every `AppendBatch`.
    std::unordered_map<int, std::unordered_map<loglib::LogLevel, std::vector<std::string>>>
        mLastBatchLevelDemoteMapping;
};

Q_DECLARE_METATYPE(StreamingResult)
Q_DECLARE_METATYPE(EnumColumnsChangeReason)
Q_DECLARE_METATYPE(loglib::SourceStatus)
