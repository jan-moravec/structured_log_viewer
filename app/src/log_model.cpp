#include "log_model.hpp"

#include "qt_streaming_log_sink.hpp"
#include "streaming_control.hpp"

#include <loglib/file_line_source.hpp>
#include <loglib/parsers/json_parser.hpp>
#include <loglib/line_source.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/bytes_producer.hpp>
#include <loglib/parser_options.hpp>
#include <loglib/stream_line_source.hpp>
#include <loglib/log_parse_sink.hpp>

#include <QCoreApplication>
#include <QFutureWatcher>
#include <QMetaObject>
#include <QModelIndex>
#include <QPointer>
#include <QString>
#include <QThread>
#include <QVariant>
#include <QtConcurrent/QtConcurrent>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

LogModel::LogModel(QObject *parent) : QAbstractTableModel{parent}
{
    qRegisterMetaType<StreamingResult>("StreamingResult");
    // Required so the `sourceStatusChanged` signal can be delivered
    // across the worker→GUI thread hop via queued invocation (Qt
    // refuses to queue values of unregistered types).
    qRegisterMetaType<loglib::SourceStatus>("loglib::SourceStatus");

    mSink = new QtStreamingLogSink(this, this);
    mStreamingWatcher = new QFutureWatcher<void>(this);
}

LogModel::~LogModel()
{
    // Mandatory teardown order from :
    //   1. Producer `Stop()` so any blocking Read / WaitForBytes returns.
    //   2. `mSink->RequestStop()` triggers the parser's stopToken; the
    //      hot loop sees it on the next batch boundary.
    //   3. `mStreamingWatcher->waitForFinished()` joins the worker so
    //      the long-lived `LineSource` installed in `mLogTable` outlives
    //      every in-flight Stage B / RunStreamingParser call.
    //   4. `mSink->DropPendingBatches()` bumps the generation so any
    //      drain-phase queued lambdas short-circuit.
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
        // GUI-thread-only contract; non-GUI callers would deadlock on a
        // queued `OnFinished` that needs the event loop.
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
    // Stop "leaves the most-recently-buffered rows visible".
    // Same teardown sequence as `Reset()` minus the model reset — paused
    // rows are still flushed into the visible model so Stop never
    // silently discards parsed content.
    TeardownStreamingSessionInternal(/*resetTable=*/false);
}

void LogModel::TeardownStreamingSessionInternal(bool resetTable)
{
    // `mStreamingActive` (GUI-thread-only) is the source of truth for
    // "still streaming"; `isRunning()` flips off before the queued
    // `OnFinished` reaches the GUI thread.
    const bool wasStreaming = mStreamingActive;
    mStreamingActive = false;

    // Step 1: release I/O so a worker parked in `Read`/`WaitForBytes`
    // returns immediately.
    if (loglib::BytesProducer *producer = ActiveProducer(); producer != nullptr)
    {
        producer->Stop();
    }
    // Step 2: cooperative parser stop (separate from the I/O stop above).
    if (mSink)
    {
        mSink->RequestStop();
    }
    // Step 3: join the worker before tearing the LogTable/LogFile down.
    if (mStreamingWatcher)
    {
        Q_ASSERT(QThread::currentThread() == thread());
        mStreamingWatcher->waitForFinished();
    }

    // Step 3.4 (StopAndKeepRows only): drain the sink's queued meta-call
    // events so the drain-phase `OnBatch` / `OnFinished` lambdas the
    // worker posted between `RequestStop()` and `waitForFinished()`
    // returning land their rows in the visible model under the
    // still-valid generation. `Reset()` deliberately skips this step --
    // the table is reset below anyway, and draining there would emit
    // spurious `streamingFinished` / `lineCountChanged` against
    // soon-to-be-reset state, breaking the existing
    // `testResetDuringStreamingDropsDrainPhaseBatch` contract
    // (drain-phase rows are dropped on the Reset path).
    //
    // After the drain, `mSink->IsActive()` flips to `false` iff the
    // queued `OnFinished` lambda ran (it sets `mActive = false` and
    // calls `EndStreaming` which already emits `streamingFinished`).
    // Use that as the sentinel to suppress the compensating emit at the
    // end of this function so observers see exactly one terminal signal.
    bool finishedAlreadyEmitted = false;
    if (!resetTable && mSink && mSink->IsActive())
    {
        QCoreApplication::sendPostedEvents(mSink, 0);
        finishedAlreadyEmitted = !mSink->IsActive();
    }

    // Step 3.5: any rows still in the paused buffer at this point are
    // already-parsed lines that the user expects to see. Flush them into
    // the visible model first so Stop never silently discards parsed
    // content. In the `Reset()` path the rows land in the model briefly
    // and are then wiped by the reset below; the flush is still kept
    // honest because observers listening for `lineCountChanged` see a
    // final count before the reset-to-zero, and in the
    // `StopAndKeepRows()` path these are the rows the user expects to
    // keep.
    if (mSink)
    {
        if (auto pending = mSink->TakePausedBuffer())
        {
            AppendBatch(std::move(*pending));
        }
    }

    // Step 4: bump generation **after** join: drain-phase
    // `OnBatch`/`OnFinished` captured the previous generation; bumping here
    // makes their queued lambdas short-circuit instead of running against
    // the reset model (use-after-free on the dangling `LineSource*` +
    // spurious second signal).
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

        endResetModel();

        emit lineCountChanged(0);
        emit errorCountChanged(0);
    }

    // The worker's queued `OnFinished(true)` is now generation-stale and
    // discarded; emit a compensating `streamingFinished` so UI gating
    // (e.g. configuration menus) re-opens.
    //
    // On the StopAndKeepRows path the drain at Step 3.4 may already have delivered
    // the queued `OnFinished` lambda (which calls `EndStreaming` and emits
    // `streamingFinished` itself). `finishedAlreadyEmitted` dedupes so the
    // observer always sees exactly one terminal signal.
    if (wasStreaming && !finishedAlreadyEmitted)
    {
        emit streamingFinished(StreamingResult::Cancelled);
    }
}

void LogModel::BeginStreamingShared(std::unique_ptr<loglib::LineSource> source)
{
    beginResetModel();

    // FileLineSource fast path: pre-reserve per-line offsets based on
    // the mmap size so per-batch offset inserts stay amortised O(1).
    // Streaming sources have no fixed length; the hint is a no-op for
    // them anyway, so no branch is needed past the dynamic_cast probe.
    std::optional<size_t> reserveCount;
    if (auto *fileSource = dynamic_cast<loglib::FileLineSource *>(source.get()); fileSource != nullptr)
    {
        // ~100 bytes/line matches the benchmark fixture.
        reserveCount = fileSource->File().Size() / 100;
    }

    mLogTable.BeginStreaming(std::move(source));
    if (reserveCount.has_value())
    {
        mLogTable.ReserveLineOffsets(*reserveCount);
    }

    mErrorCount = 0;
    mStreamingErrors.clear();

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

/// Wraps @p workerBody in the standard parser-worker boundary handler.
/// Catches both `std::exception` and unknown exceptions, converting
/// them into a synthetic terminal `OnBatch` (with a "Streaming parse
/// failed: ..." error) plus `OnFinished(false)` so the GUI watchdog
/// always observes a `finished()` regardless of how the worker exited.
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
        // No callable: synchronous-driven test; leave the watcher idle.
        return stopToken;
    }

    QtStreamingLogSink *sinkForWorker = mSink;
    auto callable = std::move(parseCallable);
    QFuture<void> future = QtConcurrent::run([sinkForWorker, stopToken, callable = std::move(callable)]() {
        RunParserWorkerWithBoundary(sinkForWorker, [&] { callable(stopToken); });
    });

    mStreamingWatcher->setFuture(std::move(future));
    return stopToken;
}

loglib::StopToken LogModel::AppendStreaming(
    std::unique_ptr<loglib::FileLineSource> source, std::function<void(loglib::StopToken)> parseCallable
)
{
    Q_ASSERT(source);
    Q_ASSERT(mStreamingWatcher == nullptr || !mStreamingWatcher->isRunning());

    // Reserve before splicing the source in: ~100 bytes/line matches the
    // benchmark fixture and keeps per-batch line-offset insertions
    // amortised O(1).
    const size_t reserveCount = source->File().Size() / 100;
    mLogTable.AppendStreaming(std::move(source));
    mLogTable.ReserveLineOffsets(reserveCount);

    // Re-arm the sink for the new worker without resetting accumulated
    // counters or wiping rows. `Arm()` bumps the generation so any
    // straggler events from the previous file's parser short-circuit.
    const loglib::StopToken stopToken = mSink->Arm();
    mStreamingActive = true;

    if (!parseCallable)
    {
        return stopToken;
    }

    QtStreamingLogSink *sinkForWorker = mSink;
    auto callable = std::move(parseCallable);
    QFuture<void> future = QtConcurrent::run([sinkForWorker, stopToken, callable = std::move(callable)]() {
        RunParserWorkerWithBoundary(sinkForWorker, [&] { callable(stopToken); });
    });

    mStreamingWatcher->setFuture(std::move(future));
    return stopToken;
}

loglib::StopToken LogModel::BeginStreaming(
    std::unique_ptr<loglib::StreamLineSource> source, loglib::ParserOptions options
)
{
    Q_ASSERT(source);
    Q_ASSERT(mStreamingWatcher == nullptr || !mStreamingWatcher->isRunning());

    // Subscribe to the producer's rotation / status hooks before handing
    // the source off. The callbacks fire from the
    // producer's worker thread; we re-emit via a queued connection so the
    // GUI sees them on the model's thread. `QPointer` makes a model
    // destruction mid-hop a graceful no-op.
    QPointer<LogModel> self(this);
    if (loglib::BytesProducer *producer = source->Producer(); producer != nullptr)
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

    QtStreamingLogSink *sinkForWorker = mSink;
    loglib::StreamLineSource *streamSourcePtr = source.get();
    BeginStreamingShared(std::move(source));

    const loglib::StopToken stopToken = mSink->Arm();
    options.stopToken = stopToken;

    // Apply the configured retention cap to the sink's paused buffer.
    // Default to `StreamingControl::kDefaultRetentionLines` when the
    // model has not been configured yet -- the live-tail entry point
    // should never be unbounded.
    if (mRetentionCap == 0)
    {
        mRetentionCap = StreamingControl::kDefaultRetentionLines;
    }
    mSink->SetRetentionCap(mRetentionCap);

    QFuture<void> future =
        QtConcurrent::run([sinkForWorker, streamSourcePtr, capturedOptions = std::move(options)]() mutable {
            RunParserWorkerWithBoundary(sinkForWorker, [&] {
                loglib::JsonParser parser;
                parser.ParseStreaming(*streamSourcePtr, *sinkForWorker, std::move(capturedOptions));
            });
        });

    mStreamingWatcher->setFuture(std::move(future));
    return stopToken;
}

void LogModel::AppendBatch(loglib::StreamedBatch batch)
{
    // Capture errors before `LogTable::AppendBatch` swallows them.
    const qsizetype capturedErrorCount = static_cast<qsizetype>(batch.errors.size());
    if (!batch.errors.empty())
    {
        mStreamingErrors.reserve(mStreamingErrors.size() + batch.errors.size());
        for (auto &error : batch.errors)
        {
            mStreamingErrors.emplace_back(std::move(error));
        }
        batch.errors.clear();
    }

    // Giant-batch collapse: if the batch alone exceeds
    // the retention cap, drop the head of the batch *before* it lands in
    // `LogTable`, so per-batch eviction stays O(cap) and the visible
    // model never breaches the cap. Live-tail batches arrive through
    // `BeginStreaming(unique_ptr<StreamLineSource>, ...)` with
    // `mRetentionCap` set to a sane default; the static-file
    // `BeginStreaming(unique_ptr<FileLineSource>, ...)` entry leaves it
    // at `0`, so finite parses never trigger the collapse.
    if (mRetentionCap != 0 && batch.lines.size() > mRetentionCap)
    {
        const size_t toDrop = batch.lines.size() - mRetentionCap;
        batch.lines.erase(batch.lines.begin(), batch.lines.begin() + static_cast<std::ptrdiff_t>(toDrop));
        batch.firstLineNumber += toDrop;
    }

    const int oldRowCount = static_cast<int>(mLogTable.RowCount());
    const int oldColumnCount = static_cast<int>(mLogTable.ColumnCount());

    // Qt's begin-before-mutate contract: predict the post-mutation counts,
    // fire `beginInsert*`, then commit. Columns first so headers are live
    // when inserted rows query `data()`.
    const auto preview = mLogTable.PreviewAppend(batch);
    int newColumnCount = static_cast<int>(preview.newColumnCount);
    int newRowCount = static_cast<int>(preview.newRowCount);

    // FIFO eviction: if the predicted row count
    // exceeds the cap, drop the oldest rows from the model *before*
    // inserting the new batch, so the visible model stays within the
    // cap. Order is `remove` → `insert` (mirrors the ).
    int dropCount = 0;
    if (mRetentionCap != 0 && newRowCount > 0 && static_cast<size_t>(newRowCount) > mRetentionCap)
    {
        dropCount = newRowCount - static_cast<int>(mRetentionCap);
        if (dropCount > oldRowCount)
        {
            // Defensive: never try to drop more than we currently have.
            // Should not fire because the giant-batch collapse above
            // already trims the batch to <= cap, but the static-path
            // (LogLine) batches don't go through that branch.
            dropCount = oldRowCount;
        }
    }

    if (dropCount > 0)
    {
        beginRemoveRows(QModelIndex(), 0, dropCount - 1);
        mLogTable.EvictPrefixRows(static_cast<size_t>(dropCount));
        endRemoveRows();
        // Adjust predictions to the post-eviction shape.
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

    mLogTable.AppendBatch(std::move(batch));

    if (rowsGrew)
    {
        endInsertRows();
    }
    if (columnsGrew)
    {
        endInsertColumns();
    }

    if (const auto &range = mLogTable.LastBackfillRange(); range.has_value() && newRowCount > 0)
    {
        const int firstColumn = static_cast<int>(range->first);
        const int lastColumn = static_cast<int>(range->second);
        emit dataChanged(
            index(0, firstColumn),
            index(newRowCount - 1, lastColumn),
            {Qt::DisplayRole, static_cast<int>(LogModelItemDataRole::SortRole)}
        );
    }

    // Match the synchronous-parse path's `LogConfigurationManager::Update`
    // semantics: every freshly-appended `Type::time` column is bubbled to
    // position 0, so the latest-discovered timestamp ends up first. The
    // append-only inserts above keep Qt's `beginInsertColumns` contract intact;
    // the reorder is a separate Qt-aware step using `beginMoveColumns`.
    if (columnsGrew)
    {
        const auto &columns = mLogTable.Configuration().Configuration().columns;
        std::vector<int> newTimestampColumnIndices;
        for (int columnIndex = oldColumnCount; columnIndex < newColumnCount; ++columnIndex)
        {
            if (columns[static_cast<size_t>(columnIndex)].type == loglib::LogConfiguration::Type::time)
            {
                newTimestampColumnIndices.push_back(columnIndex);
            }
        }
        // Process low-to-high: each move targets position 0, and because every
        // source index is > 0 the still-unprocessed indices (which are higher)
        // do not shift.
        for (int srcIndex : newTimestampColumnIndices)
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
    // The sink's bool only distinguishes clean finish vs. stop_token;
    // `StreamingResult::Failed` is a follow-up wiring task.
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
        return QVariant();
    }

    if (orientation == Qt::Horizontal && section >= 0 && section < columnCount())
    {
        return QString::fromStdString(mLogTable.GetHeader(static_cast<size_t>(section)));
    }

    return QVariant();
}

QVariant LogModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= rowCount() || index.column() >= columnCount())
    {
        return QVariant();
    }

    if (role == Qt::DisplayRole)
    {
        return ConvertToSingleLineCompactQString(
            mLogTable.GetFormattedValue(static_cast<size_t>(index.row()), static_cast<size_t>(index.column()))
        );
    }
    else if (role == LogModelItemDataRole::SortRole)
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
                    // Materialise: the view aliases the mmap and may outlive it via QVariant.
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
                else if constexpr (std::is_same_v<T, double>)
                {
                    return QVariant(arg);
                }
                else if constexpr (std::is_same_v<T, bool>)
                {
                    return QVariant(arg);
                }
                else if constexpr (std::is_same_v<T, loglib::TimeStamp>)
                {
                    return QVariant::fromValue<qint64>(arg.time_since_epoch().count());
                }
                else if constexpr (std::is_same_v<T, std::monostate>)
                {
                    return QVariant();
                }
                else
                {
                    static_assert(std::is_same_v<T, void>, "non-exhaustive visitor!");
                }
            },
            value
        );
    }
    else if (role == LogModelItemDataRole::InsertionOrderRole)
    {
        // Source-model row index; the `StreamOrderProxyModel` sorts by
        // this role with `Qt::DescendingOrder` so the most-recently-
        // appended row lands at proxy row 0 in newest-first mode.
        // Independent of `index.column()` — sorting always yields the
        // same row order regardless of which column was nominally used
        // to drive the sort.
        return QVariant(index.row());
    }
    else if (role == LogModelItemDataRole::CopyLine)
    {
        // Branch on the row's variant. The
        // After the LogLine consolidation both static- and live-tail rows
        // route through `LineSource::RawLine(lineId)`. The `LineSource *`
        // on each `LogLine` disambiguates which storage holds the bytes
        // (mmap arena vs. per-line owned buffer).
        const size_t row = static_cast<size_t>(index.row());
        const auto &line = mLogTable.Data().Lines()[row];
        const loglib::LineSource *source = line.Source();
        const std::string raw = source != nullptr ? source->RawLine(line.LineId()) : std::string{};
        return QVariant(QString::fromStdString(raw));
    }

    return QVariant();
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
        // Idle (no active stream) or unbounded: just record the value.
        return;
    }

    const bool paused = mSink && mSink->IsPaused();
    const size_t visible = static_cast<size_t>(mLogTable.RowCount());

    if (!paused)
    {
        // Running: trim visible rows down to the new cap immediately.
        // Raising the cap has no immediate effect because we cannot
        // un-evict already-dropped rows.
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

    // Paused: leave visible rows alone. Trim the paused buffer to `cap - visible`
    // so the visible+buffered total stays within `cap`.
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
