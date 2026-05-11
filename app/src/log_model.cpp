#include "log_model.hpp"

#include "qt_streaming_log_sink.hpp"
#include "streaming_control.hpp"

#include <loglib/bytes_producer.hpp>
#include <loglib/file_line_source.hpp>
#include <loglib/line_source.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_parse_sink.hpp>
#include <loglib/parser_options.hpp>
#include <loglib/parsers/json_parser.hpp>
#include <loglib/stream_line_source.hpp>

#include <QCoreApplication>
#include <QFutureWatcher>
#include <QMetaObject>
#include <QModelIndex>
#include <QPointer>
#include <QString>
#include <QThread>
#include <QVariant>
#include <QtConcurrent/QtConcurrent>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

LogModel::LogModel(QObject *parent)
    : LogModel(parent, QtStreamingLogSink::PENDING_CAPACITY_DEFAULT)
{
}

LogModel::LogModel(QObject *parent, std::size_t pendingCapacity)
    : QAbstractTableModel{parent}
{
    qRegisterMetaType<StreamingResult>("StreamingResult");
    // Required for queued worker→GUI delivery.
    qRegisterMetaType<loglib::SourceStatus>("loglib::SourceStatus");

    mSink = new QtStreamingLogSink(this, this, pendingCapacity);
    mStreamingWatcher = new QFutureWatcher<void>(this);
}

LogModel::~LogModel()
{
    // Teardown order: producer stop → sink stop → join worker →
    // bump generation so any leftover queued lambdas short-circuit.
    if (loglib::BytesProducer *producer = ActiveProducer(); producer != nullptr)
    {
        producer->Stop();
    }
    if (mSink)
    {
        mSink->RequestStop();
    }
    if (mStreamingWatcher)
    {
        // GUI-thread only; queued `OnFinished` would deadlock otherwise.
        Q_ASSERT(QThread::currentThread() == thread());
        mStreamingWatcher->waitForFinished();
    }
    if (mSink)
    {
        mSink->DropPendingBatches();
    }
}

void LogModel::Reset()
{
    TeardownStreamingSessionInternal(/*resetTable=*/true);
}

void LogModel::StopAndKeepRows()
{
    TeardownStreamingSessionInternal(/*resetTable=*/false);
}

void LogModel::TeardownStreamingSessionInternal(bool resetTable)
{
    // GUI-thread source of truth: the watcher's `isRunning()` flips off
    // before the queued `OnFinished` reaches the GUI thread.
    const bool wasStreaming = mStreamingActive;
    mStreamingActive = false;

    if (loglib::BytesProducer *producer = ActiveProducer(); producer != nullptr)
    {
        producer->Stop();
    }
    if (mSink)
    {
        mSink->RequestStop();
    }
    if (mStreamingWatcher)
    {
        Q_ASSERT(QThread::currentThread() == thread());
        mStreamingWatcher->waitForFinished();
    }

    // StopAndKeepRows: drain queued sink events under the still-valid
    // generation so straggler batches land. `Reset()` skips this --
    // the table is wiped below. `IsActive()` flips false iff
    // `OnFinished` ran (and already emitted `streamingFinished`).
    bool finishedAlreadyEmitted = false;
    if (!resetTable && mSink && mSink->IsActive())
    {
        QCoreApplication::sendPostedEvents(mSink, 0);
        // Backstop for batches enqueued before the worker exited.
        mSink->DrainNow();
        finishedAlreadyEmitted = !mSink->IsActive();
    }

    // Flush paused-buffer rows so Stop never silently discards them.
    if (mSink)
    {
        if (auto pending = mSink->TakePausedBuffer())
        {
            AppendBatch(std::move(*pending));
        }
    }

    if (mSink)
    {
        mSink->DropPendingBatches();
    }

    if (resetTable)
    {
        beginResetModel();

        mLogTable.Reset();
        mErrorCount = 0;
        mStreamingErrors.clear();
        mLastReportedShutdownDropCount = 0;

        endResetModel();

        emit lineCountChanged(0);
        emit errorCountChanged(0);
    }

    // Compensate for the worker's now generation-stale `OnFinished`
    // so UI gating reopens. `finishedAlreadyEmitted` dedupes the
    // StopAndKeepRows drain path.
    if (wasStreaming && !finishedAlreadyEmitted)
    {
        emit streamingFinished(StreamingResult::Cancelled);
    }
}

void LogModel::BeginStreamingShared(std::unique_ptr<loglib::LineSource> source)
{
    beginResetModel();

    // FileLineSource fast path: pre-reserve per-line offsets to keep
    // per-batch inserts amortised O(1). No-op for stream sources.
    constexpr size_t BYTES_PER_LINE_RESERVE_HINT = 100;
    std::optional<size_t> reserveCount;
    if (auto *fileSource = dynamic_cast<loglib::FileLineSource *>(source.get()); fileSource != nullptr)
    {
        reserveCount = fileSource->File().Size() / BYTES_PER_LINE_RESERVE_HINT;
    }

    mLogTable.BeginStreaming(std::move(source));
    if (reserveCount.has_value())
    {
        mLogTable.ReserveLineOffsets(*reserveCount);
    }

    mErrorCount = 0;
    mStreamingErrors.clear();
    mLastReportedShutdownDropCount = 0;

    // Every new session starts unbounded. Live-tail re-applies its
    // retention cap after returning; static paths leave it at 0.
    mRetentionCap = 0;
    if (mSink)
    {
        mSink->SetRetentionCap(0);
    }

    endResetModel();

    emit lineCountChanged(0);
    emit errorCountChanged(0);

    mStreamingActive = true;
}

loglib::BytesProducer *LogModel::ActiveProducer() noexcept
{
    if (loglib::StreamLineSource *streamSource = mLogTable.Data().FrontStreamSource(); streamSource != nullptr)
    {
        return streamSource->Producer();
    }
    return nullptr;
}

namespace
{

/// Run @p workerBody and turn any escaping exception into a synthetic
/// terminal `OnBatch` + `OnFinished(false)` so the GUI watchdog always
/// observes a `finished()` regardless of how the worker exited.
template <class Body> void RunParserWorkerWithBoundary(QtStreamingLogSink *sink, Body &&workerBody)
{
    try
    {
        std::forward<Body>(workerBody)();
    }
    catch (const std::exception &e)
    {
        loglib::StreamedBatch errorBatch;
        errorBatch.errors.emplace_back(std::string("Streaming parse failed: ") + e.what());
        sink->OnBatch(std::move(errorBatch));
        sink->OnFinished(false);
    }
    catch (...)
    {
        loglib::StreamedBatch errorBatch;
        errorBatch.errors.emplace_back("Streaming parse failed: unknown exception");
        sink->OnBatch(std::move(errorBatch));
        sink->OnFinished(false);
    }
}

} // namespace

loglib::StopToken LogModel::BeginStreamingForSyncTest(std::unique_ptr<loglib::LineSource> source)
{
    Q_ASSERT(mStreamingWatcher == nullptr || !mStreamingWatcher->isRunning());

    BeginStreamingShared(std::move(source));

    return mSink->Arm();
}

loglib::StopToken LogModel::BeginStreaming(
    std::unique_ptr<loglib::FileLineSource> source, std::function<void(loglib::StopToken)> parseCallable
)
{
    const loglib::StopToken stopToken = BeginStreamingForSyncTest(std::move(source));

    if (!parseCallable)
    {
        // Synchronous-driven test: leave the watcher idle.
        return stopToken;
    }

    QtStreamingLogSink *sinkForWorker = mSink;
    auto callable = std::move(parseCallable);
    const QFuture<void> future = QtConcurrent::run([sinkForWorker, stopToken, callable = std::move(callable)]() {
        RunParserWorkerWithBoundary(sinkForWorker, [&] { callable(stopToken); });
    });

    mStreamingWatcher->setFuture(future);
    return stopToken;
}

loglib::StopToken LogModel::AppendStreaming(
    std::unique_ptr<loglib::FileLineSource> source, std::function<void(loglib::StopToken)> parseCallable
)
{
    Q_ASSERT(source);
    Q_ASSERT(mStreamingWatcher == nullptr || !mStreamingWatcher->isRunning());

    // Reserve before splicing in to keep per-batch offset inserts O(1).
    const size_t reserveCount = source->File().Size() / 100;
    mLogTable.AppendStreaming(std::move(source));
    mLogTable.ReserveLineOffsets(reserveCount);

    // Re-arm the sink for the new worker without resetting counters or
    // wiping rows; `Arm()` bumps the generation.
    const loglib::StopToken stopToken = mSink->Arm();
    mStreamingActive = true;

    if (!parseCallable)
    {
        return stopToken;
    }

    QtStreamingLogSink *sinkForWorker = mSink;
    auto callable = std::move(parseCallable);
    const QFuture<void> future = QtConcurrent::run([sinkForWorker, stopToken, callable = std::move(callable)]() {
        RunParserWorkerWithBoundary(sinkForWorker, [&] { callable(stopToken); });
    });

    mStreamingWatcher->setFuture(future);
    return stopToken;
}

loglib::StopToken LogModel::BeginStreaming(
    std::unique_ptr<loglib::StreamLineSource> source, loglib::ParserOptions options
)
{
    Q_ASSERT(source);
    Q_ASSERT(mStreamingWatcher == nullptr || !mStreamingWatcher->isRunning());

    QtStreamingLogSink *sinkForWorker = mSink;
    loglib::StreamLineSource *streamSourcePtr = source.get();
    BeginStreamingShared(std::move(source));

    // Arm the sink + apply retention BEFORE wiring producer callbacks
    // so a synchronous status replay lands on a fully-initialised model.
    const loglib::StopToken stopToken = mSink->Arm();
    options.stopToken = stopToken;

    // Live-tail is never unbounded: load the user's retention from
    // `StreamingControl` (fallback to `DEFAULT_RETENTION_LINES`).
    size_t cap = StreamingControl::RetentionLines();
    if (cap == 0)
    {
        cap = StreamingControl::DEFAULT_RETENTION_LINES;
    }
    mRetentionCap = cap;
    mSink->SetRetentionCap(mRetentionCap);

    // Subscribe to producer rotation/status hooks. Callbacks fire from
    // the producer thread; re-emit queued so the GUI sees them on the
    // model's thread. `QPointer` handles destruction mid-hop.
    const QPointer<LogModel> self(this);
    if (loglib::BytesProducer *producer = streamSourcePtr->Producer(); producer != nullptr)
    {
        producer->SetRotationCallback([self]() {
            if (!self)
            {
                return;
            }
            QMetaObject::invokeMethod(
                self.data(),
                [self]() {
                    if (self)
                    {
                        emit self->rotationDetected();
                    }
                },
                Qt::QueuedConnection
            );
        });
        producer->SetStatusCallback([self](loglib::SourceStatus status) {
            if (!self)
            {
                return;
            }
            QMetaObject::invokeMethod(
                self.data(),
                [self, status]() {
                    if (self)
                    {
                        emit self->sourceStatusChanged(status);
                    }
                },
                Qt::QueuedConnection
            );
        });
    }

    const QFuture<void> future =
        QtConcurrent::run([sinkForWorker, streamSourcePtr, capturedOptions = std::move(options)]() mutable {
            RunParserWorkerWithBoundary(sinkForWorker, [&] {
                const loglib::JsonParser parser;
                parser.ParseStreaming(*streamSourcePtr, *sinkForWorker, std::move(capturedOptions));
            });
        });

    mStreamingWatcher->setFuture(future);
    return stopToken;
}

void LogModel::AppendBatch(loglib::StreamedBatch batch)
{
    // Capture errors before `LogTable::AppendBatch` swallows them.
    const auto capturedErrorCount = static_cast<qsizetype>(batch.errors.size());
    if (!batch.errors.empty())
    {
        mStreamingErrors.reserve(mStreamingErrors.size() + batch.errors.size());
        for (auto &error : batch.errors)
        {
            mStreamingErrors.emplace_back(std::move(error));
        }
        batch.errors.clear();
    }

    // Giant-batch collapse: drop the head before it lands in `LogTable`
    // so per-batch eviction stays O(cap). Skipped when cap == 0.
    if (mRetentionCap != 0 && batch.lines.size() > mRetentionCap)
    {
        const size_t toDrop = batch.lines.size() - mRetentionCap;
        batch.lines.erase(batch.lines.begin(), batch.lines.begin() + static_cast<std::ptrdiff_t>(toDrop));
        batch.firstLineNumber += toDrop;
    }

    const int oldRowCount = static_cast<int>(mLogTable.RowCount());
    const int oldColumnCount = static_cast<int>(mLogTable.ColumnCount());

    // Qt's begin-before-mutate contract: predict counts, fire
    // `beginInsert*`, then commit. Columns first so headers are live
    // when inserted rows query `data()`.
    const auto preview = mLogTable.PreviewAppend(batch);
    const int newColumnCount = static_cast<int>(preview.newColumnCount);
    int newRowCount = static_cast<int>(preview.newRowCount);

    // FIFO eviction: drop oldest rows before inserting the new batch
    // (remove → insert).
    int dropCount = 0;
    if (mRetentionCap != 0 && newRowCount > 0 && std::cmp_greater(newRowCount, mRetentionCap))
    {
        dropCount = newRowCount - static_cast<int>(mRetentionCap);
        dropCount = std::min(dropCount, oldRowCount);
    }

    if (dropCount > 0)
    {
        beginRemoveRows(QModelIndex(), 0, dropCount - 1);
        mLogTable.EvictPrefixRows(static_cast<size_t>(dropCount));
        endRemoveRows();
        newRowCount -= dropCount;
    }

    const int currentRowCount = static_cast<int>(mLogTable.RowCount());
    const bool columnsGrew = newColumnCount > oldColumnCount;
    const bool rowsGrew = newRowCount > currentRowCount;

    if (columnsGrew)
    {
        beginInsertColumns(QModelIndex(), oldColumnCount, newColumnCount - 1);
    }
    if (rowsGrew)
    {
        beginInsertRows(QModelIndex(), currentRowCount, newRowCount - 1);
    }

    // Snapshot pre-batch dict sizes for active enum columns. If a batch
    // grows any, emit `enumColumnsChanged` so `MainWindow` rebuilds the
    // rules and `loglib::EnumRowPredicate`'s bitset stays current.
    std::vector<std::pair<loglib::KeyId, uint16_t>> enumDictSizesBefore;
    {
        const auto &columnsBefore = mLogTable.Configuration().Configuration().columns;
        const auto &keysBefore = mLogTable.Keys();
        const auto &registryBefore = mLogTable.EnumDictionaries();
        for (const auto &column : columnsBefore)
        {
            if (column.type != loglib::LogConfiguration::Type::Enumeration || column.keys.empty())
            {
                continue;
            }
            const loglib::KeyId kid = keysBefore.Find(column.keys.front());
            if (kid == loglib::INVALID_KEY_ID)
            {
                continue;
            }
            if (const loglib::EnumDictionary *dict = registryBefore.Find(kid); dict != nullptr)
            {
                enumDictSizesBefore.emplace_back(kid, dict->Size());
            }
        }
    }

    mLogTable.AppendBatch(std::move(batch));

    bool enumDictionariesGrew = false;
    if (!enumDictSizesBefore.empty())
    {
        const auto &registryAfter = mLogTable.EnumDictionaries();
        for (const auto &[kid, sizeBefore] : enumDictSizesBefore)
        {
            const loglib::EnumDictionary *dict = registryAfter.Find(kid);
            if (dict != nullptr && dict->Size() > sizeBefore)
            {
                enumDictionariesGrew = true;
                break;
            }
        }
    }

    if (rowsGrew)
    {
        endInsertRows();
    }
    if (columnsGrew)
    {
        endInsertColumns();
    }

    bool emittedEnumColumnsChanged = false;
    if (const auto &range = mLogTable.LastBackfillRange(); range.has_value() && newRowCount > 0)
    {
        const int firstColumn = static_cast<int>(range->first);
        const int lastColumn = static_cast<int>(range->second);
        // Include `EnumValueRole`: enum-promotion back-fill produces
        // fresh `DictRef` slots even when the display string is unchanged.
        emit dataChanged(
            index(0, firstColumn),
            index(newRowCount - 1, lastColumn),
            {Qt::DisplayRole,
             static_cast<int>(LogModelItemDataRole::SortRole),
             static_cast<int>(LogModelItemDataRole::EnumValueRole)}
        );

        // Notify if any back-filled column is now an enumeration.
        const auto &columns = mLogTable.Configuration().Configuration().columns;
        for (int columnIndex = firstColumn; columnIndex <= lastColumn; ++columnIndex)
        {
            if (columnIndex < 0 || std::cmp_greater_equal(columnIndex, columns.size()))
            {
                continue;
            }
            if (columns[static_cast<size_t>(columnIndex)].type == loglib::LogConfiguration::Type::Enumeration)
            {
                emit enumColumnsChanged();
                emittedEnumColumnsChanged = true;
                break;
            }
        }
    }

    // Dictionaries that grew without a promotion still need a refresh
    // so `loglib::EnumRowPredicate`'s bitset covers any newly-interned id.
    if (!emittedEnumColumnsChanged && enumDictionariesGrew)
    {
        emit enumColumnsChanged();
    }

    // Match `LogConfigurationManager::Update`: bubble each freshly
    // appended `Type::Time` column to position 0.
    if (columnsGrew)
    {
        const auto &columns = mLogTable.Configuration().Configuration().columns;
        std::vector<int> newTimestampColumnIndices;
        for (int columnIndex = oldColumnCount; columnIndex < newColumnCount; ++columnIndex)
        {
            if (columns[static_cast<size_t>(columnIndex)].type == loglib::LogConfiguration::Type::Time)
            {
                newTimestampColumnIndices.push_back(columnIndex);
            }
        }
        // Process low-to-high so unprocessed (higher) indices don't shift.
        for (const int srcIndex : newTimestampColumnIndices)
        {
            if (srcIndex == 0)
            {
                continue;
            }
            if (beginMoveColumns(QModelIndex(), srcIndex, srcIndex, QModelIndex(), 0))
            {
                mLogTable.MoveColumn(static_cast<size_t>(srcIndex), 0);
                endMoveColumns();
            }
        }
    }

    mErrorCount += capturedErrorCount;
    emit lineCountChanged(static_cast<qsizetype>(newRowCount));
    if (capturedErrorCount > 0)
    {
        emit errorCountChanged(mErrorCount);
    }
}

void LogModel::EndStreaming(bool cancelled)
{
    mStreamingActive = false;

    // Graceful end-of-stream: run the permissive auto-detection sweep so
    // small/short streams still get enum filter chips. Cancellation
    // skips the sweep. Emit `enumColumnsChanged` only when something
    // actually promoted, before `streamingFinished` so the picker has
    // a stable dictionary by the time the UI re-enables editing.
    if (!cancelled)
    {
        if (mLogTable.FinalizeAutoDetection())
        {
            emit enumColumnsChanged();
        }
    }

    // `StreamingResult::Failed` is wired up at the worker boundary.
    emit streamingFinished(cancelled ? StreamingResult::Cancelled : StreamingResult::Success);
}

int LogModel::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return static_cast<int>(mLogTable.RowCount());
}

int LogModel::columnCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return static_cast<int>(mLogTable.ColumnCount());
}

QVariant LogModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role != Qt::DisplayRole)
    {
        return {};
    }

    if (orientation == Qt::Horizontal && section >= 0 && section < columnCount())
    {
        return QString::fromStdString(mLogTable.GetHeader(static_cast<size_t>(section)));
    }

    return {};
}

QVariant LogModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= rowCount() || index.column() >= columnCount())
    {
        return {};
    }

    if (role == Qt::DisplayRole)
    {
        return ConvertToSingleLineCompactQString(
            mLogTable.GetFormattedValue(static_cast<size_t>(index.row()), static_cast<size_t>(index.column()))
        );
    }
    if (role == LogModelItemDataRole::SortRole)
    {
        loglib::LogValue value =
            mLogTable.GetValue(static_cast<size_t>(index.row()), static_cast<size_t>(index.column()));
        return std::visit(
            [](auto &&arg) -> QVariant {
                using T = std::decay_t<decltype(arg)>;
                if constexpr (std::is_same_v<T, std::string>)
                {
                    return QVariant(ConvertToSingleLineCompactQString(arg));
                }
                else if constexpr (std::is_same_v<T, std::string_view>)
                {
                    // Materialise: view aliases mmap and may outlive it.
                    return QVariant(ConvertToSingleLineCompactQString(std::string(arg)));
                }
                else if constexpr (std::is_same_v<T, int64_t>)
                {
                    return QVariant::fromValue<qlonglong>(arg);
                }
                else if constexpr (std::is_same_v<T, uint64_t>)
                {
                    return QVariant::fromValue<qulonglong>(arg);
                }
                else if constexpr (std::is_same_v<T, double> || std::is_same_v<T, bool>)
                {
                    return {arg};
                }
                else if constexpr (std::is_same_v<T, loglib::TimeStamp>)
                {
                    return QVariant::fromValue<qint64>(arg.time_since_epoch().count());
                }
                else if constexpr (std::is_same_v<T, std::monostate>)
                {
                    return {};
                }
                else
                {
                    static_assert(std::is_same_v<T, void>, "non-exhaustive visitor!");
                }
            },
            value
        );
    }
    if (role == LogModelItemDataRole::InsertionOrderRole)
    {
        return {index.row()};
    }
    if (role == LogModelItemDataRole::CopyLine)
    {
        // `LineSource *` distinguishes mmap arena vs. owned bytes.
        const auto row = static_cast<size_t>(index.row());
        const auto &line = mLogTable.Data().Lines()[row];
        const loglib::LineSource *source = line.Source();
        const std::string raw = source != nullptr ? source->RawLine(line.LineId()) : std::string{};
        return {QString::fromStdString(raw)};
    }
    if (role == LogModelItemDataRole::EnumValueRole)
    {
        // `qint32` for `DictRef` slots; invalid means the slot is
        // monostate or unpromoted. Still exposed via the data() role
        // for external consumers (tests, future widgets); the active
        // filter predicate path now reads `LogTable` directly.
        const auto enumId =
            mLogTable.GetEnumValueId(static_cast<size_t>(index.row()), static_cast<size_t>(index.column()));
        if (!enumId.has_value())
        {
            return {};
        }
        return QVariant::fromValue<qint32>(static_cast<qint32>(*enumId));
    }

    return {};
}

template <typename T> std::optional<std::pair<T, T>> LogModel::GetMinMaxValues(int column) const
{
    if (column < 0 || column >= columnCount() || rowCount() <= 0)
    {
        return std::nullopt;
    }

    const QVariant initialValue = data(index(0, column), LogModelItemDataRole::SortRole);
    if (!initialValue.isValid())
    {
        return std::nullopt;
    }

    auto minVal = initialValue.value<T>();
    auto maxVal = initialValue.value<T>();

    for (int row = 1; row < rowCount(); ++row)
    {
        const QVariant valueVariant = data(index(row, column), LogModelItemDataRole::SortRole);
        if (valueVariant.isValid())
        {
            const auto value = valueVariant.value<T>();
            if (value < minVal)
            {
                minVal = value;
            }
            if (value > maxVal)
            {
                maxVal = value;
            }
        }
    }

    return std::make_pair(minVal, maxVal);
}

template std::optional<std::pair<qint64, qint64>> LogModel::GetMinMaxValues<qint64>(int column) const;

const loglib::LogTable &LogModel::Table() const
{
    return mLogTable;
}

loglib::LogTable &LogModel::Table()
{
    return mLogTable;
}

const loglib::LogData &LogModel::Data() const
{
    return mLogTable.Data();
}

const loglib::LogConfiguration &LogModel::Configuration() const
{
    return mLogTable.Configuration().Configuration();
}

loglib::LogConfigurationManager &LogModel::ConfigurationManager()
{
    return mLogTable.Configuration();
}

QtStreamingLogSink *LogModel::Sink()
{
    return mSink;
}

const std::vector<std::string> &LogModel::StreamingErrors() const
{
    // Surface back-pressure shutdown drops as synthetic error strings
    // so the post-session dialog reports incomplete output. Materialised
    // lazily here so `mErrorCount` (per-line parse errors only) stays
    // unaffected.
    if (mSink != nullptr)
    {
        const std::size_t dropped = mSink->BatchesDroppedDuringShutdown();
        if (dropped != mLastReportedShutdownDropCount)
        {
            const std::size_t delta = dropped - mLastReportedShutdownDropCount;
            mStreamingErrors.emplace_back(
                std::string("Lost ") + std::to_string(delta) + (delta == 1 ? " parsed batch" : " parsed batches") +
                " between the parser and the GUI when the stream was stopped (back-pressure shutdown)."
            );
            mLastReportedShutdownDropCount = dropped;
        }
    }
    return mStreamingErrors;
}

bool LogModel::IsStreamingActive() const noexcept
{
    return mStreamingActive;
}

void LogModel::SetRetentionCap(size_t cap)
{
    mRetentionCap = cap;
    if (mSink)
    {
        mSink->SetRetentionCap(cap);
    }

    if (cap == 0 || !mStreamingActive)
    {
        return;
    }

    const bool paused = mSink && mSink->IsPaused();
    const size_t visible = mLogTable.RowCount();

    if (!paused)
    {
        // Running: trim visible rows down to the new cap. Raising the
        // cap has no immediate effect.
        if (visible > cap)
        {
            const size_t dropCount = visible - cap;
            beginRemoveRows(QModelIndex(), 0, static_cast<int>(dropCount) - 1);
            mLogTable.EvictPrefixRows(dropCount);
            endRemoveRows();
            emit lineCountChanged(static_cast<qsizetype>(mLogTable.RowCount()));
        }
        return;
    }

    // Paused: leave visible rows alone; trim the paused buffer so
    // visible + buffered <= cap.
    const size_t maxBuffered = (visible >= cap) ? 0 : (cap - visible);
    if (mSink)
    {
        mSink->TrimPausedBufferTo(maxBuffered);
    }
}

size_t LogModel::RetentionCap() const noexcept
{
    return mRetentionCap;
}

QString LogModel::ConvertToSingleLineCompactQString(const std::string &string)
{
    QString qString = QString::fromStdString(string);
    qString.replace("\n", " ");
    qString.replace("\r", " ");
    return qString.simplified();
}
