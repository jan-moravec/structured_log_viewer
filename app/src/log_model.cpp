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

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

LogModel::LogModel(QObject *parent)
    : QAbstractTableModel{parent}
{
    qRegisterMetaType<StreamingResult>("StreamingResult");
    // `sourceStatusChanged` crosses the worker→GUI thread boundary via
    // queued invocation; Qt refuses to queue unregistered types.
    qRegisterMetaType<loglib::SourceStatus>("loglib::SourceStatus");

    mSink = new QtStreamingLogSink(this, this);
    mStreamingWatcher = new QFutureWatcher<void>(this);
}

LogModel::~LogModel()
{
    // Mandatory teardown order:
    //   1. Producer `Stop()` so any blocking Read/WaitForBytes returns.
    //   2. Sink `RequestStop()` trips the parser's stop token.
    //   3. Join the worker so the `LineSource` outlives every
    //      in-flight parser call.
    //   4. `DropPendingBatches()` bumps the generation so drain-phase
    //      queued lambdas short-circuit.
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
        // GUI-thread only; non-GUI callers would deadlock on a queued
        // `OnFinished` that needs the event loop.
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
    // `mStreamingActive` is the GUI-thread source of truth for "still
    // streaming"; the watcher's `isRunning()` flips off before the
    // queued `OnFinished` reaches the GUI thread.
    const bool wasStreaming = mStreamingActive;
    mStreamingActive = false;

    // 1. Release I/O so a worker parked on Read/WaitForBytes returns.
    if (loglib::BytesProducer *producer = ActiveProducer(); producer != nullptr)
    {
        producer->Stop();
    }
    // 2. Cooperative parser stop (separate from the I/O stop above).
    if (mSink)
    {
        mSink->RequestStop();
    }
    // 3. Join the worker before tearing LogTable/LogFile down.
    if (mStreamingWatcher)
    {
        Q_ASSERT(QThread::currentThread() == thread());
        mStreamingWatcher->waitForFinished();
    }

    // 3a. StopAndKeepRows only: drain queued sink events so drain-phase
    // OnBatch/OnFinished lambdas land their rows under the still-valid
    // generation. `Reset()` skips this -- the table is wiped below and
    // a drain there would emit spurious signals against soon-to-be-
    // reset state. After the drain, `mSink->IsActive()` flips to false
    // iff `OnFinished` ran (which itself emits `streamingFinished`);
    // use that to suppress the compensating emit below.
    bool finishedAlreadyEmitted = false;
    if (!resetTable && mSink && mSink->IsActive())
    {
        QCoreApplication::sendPostedEvents(mSink, 0);
        finishedAlreadyEmitted = !mSink->IsActive();
    }

    // 3b. Flush already-parsed paused-buffer rows into the visible
    // model so Stop never silently discards them. On the `Reset()` path
    // these are wiped by the reset below; observers still see a final
    // `lineCountChanged` before the reset-to-zero.
    if (mSink)
    {
        if (auto pending = mSink->TakePausedBuffer())
        {
            AppendBatch(std::move(*pending));
        }
    }

    // 4. Bump generation after join so any leftover drain-phase
    // lambdas short-circuit instead of running against the reset model.
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

    // The worker's queued `OnFinished(true)` is now generation-stale;
    // emit a compensating `streamingFinished` so UI gating reopens.
    // `finishedAlreadyEmitted` dedupes against the StopAndKeepRows
    // drain so observers see exactly one terminal signal.
    if (wasStreaming && !finishedAlreadyEmitted)
    {
        emit streamingFinished(StreamingResult::Cancelled);
    }
}

void LogModel::BeginStreamingShared(std::unique_ptr<loglib::LineSource> source)
{
    beginResetModel();

    // FileLineSource fast path: pre-reserve per-line offsets so per-batch
    // inserts stay amortised O(1). ~100 bytes/line matches the benchmark
    // fixture. The hint is a no-op for stream sources.
    std::optional<size_t> reserveCount;
    if (auto *fileSource = dynamic_cast<loglib::FileLineSource *>(source.get()); fileSource != nullptr)
    {
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

    // Reserve before splicing the source in. ~100 bytes/line matches
    // the benchmark fixture and keeps per-batch offset inserts O(1).
    const size_t reserveCount = source->File().Size() / 100;
    mLogTable.AppendStreaming(std::move(source));
    mLogTable.ReserveLineOffsets(reserveCount);

    // Re-arm the sink for the new worker without resetting counters or
    // wiping rows. `Arm()` bumps the generation so straggler events
    // from the previous file's parser short-circuit.
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
    // the source off. The callbacks fire from the producer's worker
    // thread; re-emit via a queued connection so the GUI receives them
    // on the model's thread. `QPointer` makes destruction mid-hop a
    // graceful no-op.
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

    // The live-tail entry point is never unbounded: fall back to the
    // default retention cap if the model has not been configured.
    if (mRetentionCap == 0)
    {
        mRetentionCap = StreamingControl::DEFAULT_RETENTION_LINES;
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

    // Giant-batch collapse: when the batch alone exceeds the cap, drop
    // its head before it lands in `LogTable` so per-batch eviction
    // stays O(cap). Static-file sessions leave `mRetentionCap == 0`, so
    // finite parses never trigger this branch.
    if (mRetentionCap != 0 && batch.lines.size() > mRetentionCap)
    {
        const size_t toDrop = batch.lines.size() - mRetentionCap;
        batch.lines.erase(batch.lines.begin(), batch.lines.begin() + static_cast<std::ptrdiff_t>(toDrop));
        batch.firstLineNumber += toDrop;
    }

    const int oldRowCount = static_cast<int>(mLogTable.RowCount());
    const int oldColumnCount = static_cast<int>(mLogTable.ColumnCount());

    // Qt's begin-before-mutate contract: predict the post-mutation
    // counts, fire `beginInsert*`, then commit. Columns first so
    // headers are live when inserted rows query `data()`.
    const auto preview = mLogTable.PreviewAppend(batch);
    int newColumnCount = static_cast<int>(preview.newColumnCount);
    int newRowCount = static_cast<int>(preview.newRowCount);

    // FIFO eviction: drop the oldest rows before inserting the new
    // batch so the visible model stays within the cap. Order is
    // remove → insert.
    int dropCount = 0;
    if (mRetentionCap != 0 && newRowCount > 0 && static_cast<size_t>(newRowCount) > mRetentionCap)
    {
        dropCount = newRowCount - static_cast<int>(mRetentionCap);
        if (dropCount > oldRowCount)
        {
            dropCount = oldRowCount;
        }
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
    // semantics: bubble each freshly appended `Type::time` column to
    // position 0 so the latest-discovered timestamp ends up first. The
    // appends above already fired `beginInsertColumns`; the reorder is
    // a separate `beginMoveColumns` step.
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
        // Process low-to-high: each move targets index 0, and because
        // every source index is > 0 the unprocessed (higher) indices do
        // not shift.
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
        // Bare source row index, column-independent (see the role's
        // declaration). `StreamOrderProxyModel` sorts by it with
        // `Qt::DescendingOrder` for newest-first mode.
        return QVariant(index.row());
    }
    else if (role == LogModelItemDataRole::CopyLine)
    {
        // The `LineSource *` on each `LogLine` disambiguates between
        // the mmap arena (file path) and per-line owned bytes
        // (live-tail path).
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
        return;
    }

    const bool paused = mSink && mSink->IsPaused();
    const size_t visible = static_cast<size_t>(mLogTable.RowCount());

    if (!paused)
    {
        // Running: trim visible rows down to the new cap. Raising has
        // no immediate effect (already-evicted rows cannot be restored).
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

    // Paused: leave visible rows alone, trim the paused buffer instead
    // so visible + buffered <= cap.
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
