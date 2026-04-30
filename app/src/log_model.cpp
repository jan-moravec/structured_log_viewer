#include "log_model.hpp"

#include "qt_streaming_log_sink.hpp"

#include <loglib/json_parser.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_source.hpp>
#include <loglib/mapped_file_source.hpp>
#include <loglib/parser_options.hpp>
#include <loglib/streaming_log_sink.hpp>

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
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

LogModel::LogModel(QObject *parent) : QAbstractTableModel{parent}
{
    qRegisterMetaType<StreamingResult>("StreamingResult");

    mSink = new QtStreamingLogSink(this, this);
    mStreamingWatcher = new QFutureWatcher<void>(this);
}

LogModel::~LogModel()
{
    // Mandatory teardown order from PRD 4.7.2.i:
    //   1. `LogSource::Stop()` so any blocking Read / WaitForBytes returns.
    //   2. `mSink->RequestStop()` triggers the parser's stopToken; the
    //      hot loop sees it on the next batch boundary.
    //   3. `mStreamingWatcher->waitForFinished()` joins the worker so
    //      the borrowed `LogFile*` (and the `mActiveSource`) outlive
    //      every in-flight Stage B / DecodeStreamLine call.
    //   4. `mSink->DropPendingBatches()` bumps the generation so any
    //      drain-phase queued lambdas short-circuit.
    if (mActiveSource)
    {
        mActiveSource->Stop();
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

void LogModel::AddData(loglib::LogData &&logData)
{
    beginResetModel();

    mLogTable.Update(std::move(logData));

    endResetModel();
}

void LogModel::Clear()
{
    // `mStreamingActive` (GUI-thread-only) is the source of truth for
    // "still streaming"; `isRunning()` flips off before the queued
    // `OnFinished` reaches the GUI thread.
    const bool wasStreaming = mStreamingActive;
    mStreamingActive = false;

    // Step 1: release I/O so a worker parked in `Read`/`WaitForBytes`
    // returns immediately (PRD 4.7.2.i).
    if (mActiveSource)
    {
        mActiveSource->Stop();
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

    // Step 3.5: any rows still in the paused buffer at this point are
    // already-parsed lines that the user expects to see. Flush them into
    // the visible model first so Stop never silently discards parsed
    // content (PRD 4.7.3) — even though the visible model is about to be
    // reset on the same path. Keeping the flush honest means the
    // streaming-errors / line counters tick before reset, so observers
    // that listen to `lineCountChanged` see a final count.
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
    // the reset model (use-after-free on the dangling `LogFile*` + spurious
    // second signal).
    if (mSink)
    {
        mSink->DropPendingBatches();
    }

    mActiveSource.reset();

    beginResetModel();

    mLogTable.Reset();
    mErrorCount = 0;
    mStreamingErrors.clear();

    endResetModel();

    emit lineCountChanged(0);
    emit errorCountChanged(0);

    // The worker's queued `OnFinished(true)` is now generation-stale and
    // discarded; emit a compensating `streamingFinished` so UI gating
    // (e.g. configuration menus) re-opens.
    if (wasStreaming)
    {
        emit streamingFinished(StreamingResult::Cancelled);
    }
}

void LogModel::BeginStreamingShared(std::unique_ptr<loglib::LogFile> file)
{
    beginResetModel();

    if (file)
    {
        // ~100 bytes/line matches the benchmark fixture; keeps per-batch
        // line-offset insertions amortised O(1).
        const size_t reserveCount = file->Size() / 100;
        mLogTable.BeginStreaming(std::move(file));
        mLogTable.ReserveLineOffsets(reserveCount);
    }
    else
    {
        mLogTable.BeginStreaming(nullptr);
    }
    mErrorCount = 0;
    mStreamingErrors.clear();

    endResetModel();

    emit lineCountChanged(0);
    emit errorCountChanged(0);

    mStreamingActive = true;
}

loglib::StopToken LogModel::BeginStreaming(std::unique_ptr<loglib::LogFile> file)
{
    Q_ASSERT(mStreamingWatcher == nullptr || !mStreamingWatcher->isRunning());

    BeginStreamingShared(std::move(file));

    return mSink->Arm();
}

loglib::StopToken LogModel::BeginStreaming(
    std::unique_ptr<loglib::LogFile> file, std::function<void(loglib::StopToken)> parseCallable
)
{
    const loglib::StopToken stopToken = BeginStreaming(std::move(file));

    if (!parseCallable)
    {
        // No callable: synchronous-driven test; leave the watcher idle.
        return stopToken;
    }

    QtStreamingLogSink *sinkForWorker = mSink;
    auto callable = std::move(parseCallable);
    QFuture<void> future = QtConcurrent::run([sinkForWorker, stopToken, callable = std::move(callable)]() {
        try
        {
            callable(stopToken);
        }
        catch (const std::exception &e)
        {
            // Synthetic terminal batch + OnFinished so the GUI watchdog
            // always observes a `finished()`.
            loglib::StreamedBatch errorBatch;
            errorBatch.errors.emplace_back(std::string("Streaming parse failed: ") + e.what());
            sinkForWorker->OnBatch(std::move(errorBatch));
            sinkForWorker->OnFinished(false);
        }
        catch (...)
        {
            loglib::StreamedBatch errorBatch;
            errorBatch.errors.emplace_back("Streaming parse failed: unknown exception");
            sinkForWorker->OnBatch(std::move(errorBatch));
            sinkForWorker->OnFinished(false);
        }
    });

    mStreamingWatcher->setFuture(std::move(future));
    return stopToken;
}

loglib::StopToken LogModel::BeginStreaming(
    std::unique_ptr<loglib::LogSource> source, loglib::ParserOptions options
)
{
    Q_ASSERT(source);
    Q_ASSERT(mStreamingWatcher == nullptr || !mStreamingWatcher->isRunning());

    // For mmap-backed sources we keep the existing static-path data flow:
    // the LogTable owns the LogFile (so `LogData::AppendBatch(lines,
    // lineOffsets)` can call `mFiles.front()->AppendLineOffsets`), and the
    // source itself becomes a *borrowing* wrapper over that file. Without
    // this re-wrap the parser's `IsMappedFile()` fast path would fall
    // through to the slow streaming loop the moment we released the file
    // out of the source.
    std::unique_ptr<loglib::LogFile> mappedFile;
    if (source->IsMappedFile())
    {
        if (auto *mapped = dynamic_cast<loglib::MappedFileSource *>(source.get()))
        {
            mappedFile = mapped->ReleaseFile();
        }
    }

    if (mappedFile)
    {
        loglib::LogFile &fileRef = *mappedFile;
        BeginStreamingShared(std::move(mappedFile));
        // Re-wrap as a borrowing source so the parser still finds the file
        // via `GetMappedLogFile()`. The borrowing ctor's `mOwnedFile` is
        // empty, so `Stop()` only flips the atomic — no double-free risk.
        source = std::make_unique<loglib::MappedFileSource>(fileRef);
    }
    else
    {
        BeginStreamingShared(nullptr);
    }

    const loglib::StopToken stopToken = mSink->Arm();
    options.stopToken = stopToken;

    // Apply the configured retention cap to the sink's paused buffer
    // (PRD 4.2.2.iv). Default to `kDefaultRetentionLines` when the model
    // has not been configured yet — the live-tail entry point should
    // never be unbounded.
    if (mRetentionCap == 0)
    {
        mRetentionCap = kDefaultRetentionLines;
    }
    mSink->SetRetentionCap(mRetentionCap);

    // Subscribe to the source's rotation hook before handing it to the
    // worker (PRD 4.8.7.v). The callback fires from the source's worker
    // thread; we re-emit via a queued connection so the GUI sees it on
    // the model's thread.
    QPointer<LogModel> self(this);
    source->SetRotationCallback([self]() {
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

    mActiveSource = std::move(source);

    QtStreamingLogSink *sinkForWorker = mSink;
    loglib::LogSource *sourcePtr = mActiveSource.get();
    QFuture<void> future =
        QtConcurrent::run([sinkForWorker, sourcePtr, capturedOptions = std::move(options)]() mutable {
            try
            {
                loglib::JsonParser parser;
                parser.ParseStreaming(*sourcePtr, *sinkForWorker, std::move(capturedOptions));
            }
            catch (const std::exception &e)
            {
                loglib::StreamedBatch errorBatch;
                errorBatch.errors.emplace_back(std::string("Streaming parse failed: ") + e.what());
                sinkForWorker->OnBatch(std::move(errorBatch));
                sinkForWorker->OnFinished(false);
            }
            catch (...)
            {
                loglib::StreamedBatch errorBatch;
                errorBatch.errors.emplace_back("Streaming parse failed: unknown exception");
                sinkForWorker->OnBatch(std::move(errorBatch));
                sinkForWorker->OnFinished(false);
            }
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

    // Giant-batch collapse (PRD 4.10.3.iii): if the batch alone exceeds
    // the retention cap, drop the head of the batch *before* it lands in
    // `LogTable`, so per-batch eviction stays O(cap) and the visible
    // model never breaches the cap. Only applies to live-tail
    // (`StreamLogLine`) batches; the static `LogLine` path is finite and
    // not subject to retention.
    if (mRetentionCap != 0 && batch.streamLines.size() > mRetentionCap)
    {
        const size_t toDrop = batch.streamLines.size() - mRetentionCap;
        batch.streamLines.erase(
            batch.streamLines.begin(), batch.streamLines.begin() + static_cast<std::ptrdiff_t>(toDrop)
        );
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

    // FIFO eviction (PRD 4.5 / 4.10.3): if the predicted row count
    // exceeds the cap, drop the oldest rows from the model *before*
    // inserting the new batch, so the visible model stays within the
    // cap. Order is `remove` → `insert` (mirrors the PRD wording).
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
    // Release the source so a follow-up `BeginStreaming` does not see a
    // stale pointer; the worker is already done by the time `OnFinished`
    // reaches the GUI thread (Stage C delivers the terminal batch from
    // the same worker that's about to exit).
    mActiveSource.reset();
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
    else if (role == LogModelItemDataRole::CopyLine)
    {
        // Branch on the row's variant (PRD 4.9.7.ii / 4.10.4.iii). The
        // logical row range is `[file rows][stream rows]`, mirroring
        // `LogTable::RowCount`'s dispatch.
        const size_t row = static_cast<size_t>(index.row());
        const size_t fileRowCount = mLogTable.Data().Lines().size();
        if (row < fileRowCount)
        {
            return QVariant(QString::fromStdString(mLogTable.Data().Lines()[row].FileReference().GetLine()));
        }
        const size_t streamRow = row - fileRowCount;
        return QVariant(QString::fromStdString(mLogTable.Data().StreamLines()[streamRow].FileReference().GetLine())
        );
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
        // Running: trim visible rows down to the new cap immediately
        // (PRD 4.5.5.i). Raising the cap has no immediate effect because
        // we cannot un-evict already-dropped rows.
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

    // Paused: leave visible rows alone (PRD 4.5.5.ii — Pause suspends FIFO
    // eviction on visible rows). Trim the paused buffer to `cap - visible`
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
