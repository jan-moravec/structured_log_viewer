#include "log_model.hpp"

#include "qt_streaming_log_sink.hpp"

#include <loglib/json_parser.hpp>
#include <loglib/parser_options.hpp>
#include <loglib/streaming_log_sink.hpp>

#include <QFutureWatcher>
#include <QModelIndex>
#include <QString>
#include <QThread>
#include <QVariant>
#include <QtConcurrent/QtConcurrent>

#include <string>
#include <utility>

LogModel::LogModel(QObject *parent) : QAbstractTableModel{parent}
{
    // Register the enum so `streamingFinished(StreamingResult)` round-trips
    // through queued connections and `QVariant::value<StreamingResult>()` in
    // the test harness. Qt's metatype registry is idempotent — calling this
    // from every constructor is cheap and guarantees registration before
    // the first emit, regardless of which translation unit creates the
    // model first.
    qRegisterMetaType<StreamingResult>("StreamingResult");

    // Sink and watcher are parented to the model so their lifetimes track
    // the model's (and so QObject's parent-cleanup deletes them after the
    // explicit wait we run in the destructor below).
    mSink = new QtStreamingLogSink(this, this);
    mStreamingWatcher = new QFutureWatcher<void>(this);
}

LogModel::~LogModel()
{
    // The background parser holds a raw `LogFile*` borrowed from `mLogTable`.
    // `mLogTable` is destroyed before `QObject::~QObject()` deletes the
    // watcher child, so unless we synchronously wait here Stage B of the TBB
    // pipeline can keep reading from the unmapped mmap after this destructor
    // body completes. `RequestStop()` alone only flips a cooperative flag.
    if (mSink)
    {
        mSink->RequestStop();
    }
    if (mStreamingWatcher)
    {
        // `QFutureWatcher::waitForFinished()` blocks the calling thread on
        // the worker's thread-pool task. The model lives on the GUI thread
        // (the watcher is a child object), so this is the GUI thread
        // joining the parser worker — which is fine here because the
        // destructor only runs from a GUI-thread teardown path
        // (`MainWindow` close / `Application::quit()`). Pin the assumption
        // explicitly so a future caller from a non-GUI thread fails loudly
        // rather than deadlocking on a queued `OnFinished` that needs the
        // GUI event loop to run.
        Q_ASSERT(QThread::currentThread() == thread());
        mStreamingWatcher->waitForFinished();
    }
    // Invalidate any drain-phase OnBatch / OnFinished lambdas the worker
    // queued between `RequestStop` and the join above: their captured
    // generation will mismatch on the GUI thread and they'll short-circuit.
    // ~QObject would also drop them by destroying the sink, but bumping
    // explicitly here keeps the contract symmetric with `Clear()`.
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
    // The background `QtConcurrent::run` worker captures a raw `LogFile*`
    // borrowed from `mLogTable`. `RequestStop()` is cooperative and only
    // halts Stage A (serial_in_order); Stage B tasks already in flight run
    // to completion in parallel mode without checking the stop_token, and
    // Stage C drains them. If we destroy `mLogTable` (which unmaps the file)
    // before the pipeline returns, those Stage B tasks read from the now-
    // unmapped mmap => use-after-free. So we *block* on the future here.
    //
    // The wait also covers the case where ungated entry points (`actionOpen`,
    // `actionOpenJsonLogs`, `dropEvent`, `LoadConfiguration`) trigger a
    // `Clear()` while a streaming parse is still running.
    //
    // `mStreamingActive` (set in `BeginStreaming`, cleared in `EndStreaming`)
    // is the source of truth here rather than `mStreamingWatcher->isRunning()`:
    // the latter flips to `false` as soon as the worker function returns,
    // before the queued `OnFinished` lambda reaches the GUI thread. If
    // `Clear()` runs in that window, `DropPendingBatches()` (below) bumps
    // the sink generation and the queued `OnFinished` is silently dropped
    // by the generation check, so we must emit a compensating
    // `streamingFinished(true)` ourselves. `mStreamingActive` is still
    // `true` at that point because both setter and clearer run on the GUI
    // thread.
    const bool wasStreaming = mStreamingActive;
    mStreamingActive = false;
    if (mSink)
    {
        mSink->RequestStop();
    }
    if (mStreamingWatcher)
    {
        // Same GUI-thread join as the destructor: `Clear()` is wired
        // exclusively to GUI-thread call sites (menu actions, drop event,
        // `LoadConfiguration`). Asserting `currentThread() == thread()`
        // here turns a "wait deadlock from a non-GUI caller" into an
        // immediate debug-time abort.
        Q_ASSERT(QThread::currentThread() == thread());
        mStreamingWatcher->waitForFinished();
    }
    // Order matters: bump the sink generation **after** the worker has
    // joined, never before. The drain phase between `RequestStop` and the
    // worker returning keeps emitting `OnBatch` / `OnFinished` (Stage C's
    // tail flush + the unconditional `OnFinished` in `RunParserPipeline`),
    // and each of those calls reads `mGeneration` to capture in its queued
    // lambda. Bumping here — *after* `waitForFinished` — means those
    // captures used the previous, now-stale generation, so the lambdas
    // short-circuit on the GUI thread instead of running `AppendBatch` /
    // `EndStreaming` against the model we are about to reset (which would
    // be a use-after-free on the dangling `LogFile*` in every batched
    // `LogLine` — observable via the `CopyLine` role on a row coming from
    // the stale batch — and a spurious second `streamingFinished(true)`).
    if (mSink)
    {
        mSink->DropPendingBatches();
    }

    beginResetModel();

    // Data-only reset: `LogTable::Reset()` drops the parsed lines but
    // preserves the user-loaded `LogConfiguration` so a `LoadConfiguration`
    // followed by `File → Open` (or drag-and-drop) does not silently revert
    // to an auto-derived layout.
    mLogTable.Reset();
    mErrorCount = 0;
    mStreamingErrors.clear();

    endResetModel();

    emit lineCountChanged(0);
    emit errorCountChanged(0);

    // The worker's queued `OnFinished(true)` is discarded by the sink's
    // generation-mismatch check (`DropPendingBatches` above bumped the
    // generation after the worker had already captured the previous one),
    // so any UI state that lives on `streamingFinished` (e.g. re-enabling
    // the configuration menus) needs to be signalled here explicitly when
    // we've cancelled an active parse.
    if (wasStreaming)
    {
        emit streamingFinished(StreamingResult::Cancelled);
    }
}

loglib::StopToken LogModel::BeginStreaming(std::unique_ptr<loglib::LogFile> file)
{
    // No active async parse: the watcher must be idle on entry. Either the
    // previous parse completed and `EndStreaming` ran, or the caller went
    // through `Clear()` (which joins on `waitForFinished` before returning).
    // A non-idle watcher here means a caller is starting a second parse
    // without joining the first — which would race the new `LogFile` mmap
    // against the old worker that still holds the previous `LogFile*`.
    Q_ASSERT(mStreamingWatcher == nullptr || !mStreamingWatcher->isRunning());

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

    // Set on the GUI thread; cleared on the GUI thread by `EndStreaming` or
    // `Clear`. Used by `Clear` to decide whether to emit a compensating
    // `streamingFinished(true)` if the queued `OnFinished` is about to be
    // dropped by the sink's generation check.
    mStreamingActive = true;

    return mSink->Arm();
}

loglib::StopToken LogModel::BeginStreaming(
    std::unique_ptr<loglib::LogFile> file, std::function<void(loglib::StopToken)> parseCallable
)
{
    // Single-arg overload owns the watcher-idle assert + state setup.
    const loglib::StopToken stopToken = BeginStreaming(std::move(file));

    if (!parseCallable)
    {
        // Convenience: no callable means "synchronous-driven test"; behave
        // like the single-arg overload, leave the watcher idle.
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
            // Mirror MainWindow::OpenJsonStreaming's symmetric tail: emit a
            // synthetic terminal batch + OnFinished(false) so the GUI's
            // streamingFinished watchdog still observes a `finished()`.
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
    // Capture errors before LogTable::AppendBatch swallows them.
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

    const int oldRowCount = static_cast<int>(mLogTable.RowCount());
    const int oldColumnCount = static_cast<int>(mLogTable.ColumnCount());

    // Qt's `QAbstractItemModel` contract: `beginInsert{Rows,Columns}` must
    // fire **before** the underlying store grows, so attached proxies (e.g.
    // `QSortFilterProxyModel`) snapshot the old size in their
    // `rowsAboutToBeInserted` slot. Predict the post-mutation counts via
    // `LogTable::PreviewAppend`, fire the begins, then commit the mutation.
    const auto preview = mLogTable.PreviewAppend(batch);
    const int newColumnCount = static_cast<int>(preview.newColumnCount);
    const int newRowCount = static_cast<int>(preview.newRowCount);
    const bool columnsGrew = newColumnCount > oldColumnCount;
    const bool rowsGrew = newRowCount > oldRowCount;

    // Columns first so new headers are live when inserted rows query data().
    if (columnsGrew)
    {
        beginInsertColumns(QModelIndex(), oldColumnCount, newColumnCount - 1);
    }
    if (rowsGrew)
    {
        beginInsertRows(QModelIndex(), oldRowCount, newRowCount - 1);
    }

    mLogTable.AppendBatch(std::move(batch));

    // End in inverse order: rows close first because we opened them last.
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
    // `cancelled` only distinguishes the two outcomes the streaming sink's
    // bool currently surfaces (clean finish vs. stop_token). The
    // `StreamingResult::Failed` case is wired through a separate code path
    // (`BeginStreaming(file, parseCallable)`'s exception-escape `catch`
    // blocks emit a synthetic terminal `OnBatch` and currently route
    // `OnFinished(false)` here — semantically a "Success" right now;
    // upgrading those handlers to emit `Failed` is a follow-up).
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
                    // Materialise so the QString factory doesn't outlive the mmap view.
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
        return QVariant(
            QString::fromStdString(mLogTable.Data().Lines()[static_cast<size_t>(index.row())].FileReference().GetLine())
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

QString LogModel::ConvertToSingleLineCompactQString(const std::string &string)
{
    QString qString = QString::fromStdString(string);
    qString.replace("\n", " ");
    qString.replace("\r", " ");
    return qString.simplified();
}
