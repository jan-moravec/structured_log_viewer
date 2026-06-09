#include "log_model.hpp"

#include "anchor_manager.hpp"
#include "icon_loader.hpp"
#include "qt_streaming_log_sink.hpp"
#include "streaming_control.hpp"
#include "theme_control.hpp"
#include "uuid_utils.hpp"

#include <loglib/bytes_producer.hpp>
#include <loglib/file_line_source.hpp>
#include <loglib/line_source.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_parse_sink.hpp>
#include <loglib/parser_options.hpp>
#include <loglib/parsers/json_parser.hpp>
#include <loglib/stream_line_source.hpp>

#include <QApplication>
#include <QBrush>
#include <QColor>
#include <QCoreApplication>
#include <QFont>
#include <QFutureWatcher>
#include <QIcon>
#include <QMetaObject>
#include <QModelIndex>
#include <QPalette>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QStyle>
#include <QThread>
#include <QVariant>
#include <QtConcurrent/QtConcurrent>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

LogModel::LogModel(QObject *parent, ThemeControl *theme, AnchorManager *anchors)
    : LogModel(parent, QtStreamingLogSink::PENDING_CAPACITY_DEFAULT, theme, anchors)
{
}

LogModel::LogModel(QObject *parent, std::size_t pendingCapacity, ThemeControl *theme, AnchorManager *anchors)
    : QAbstractTableModel{parent}, mTheme(theme), mAnchors(anchors)
{
    qRegisterMetaType<StreamingResult>("StreamingResult");
    qRegisterMetaType<EnumColumnsChangeReason>("EnumColumnsChangeReason");
    // Required for queued worker→GUI delivery.
    qRegisterMetaType<loglib::SourceStatus>("loglib::SourceStatus");

    mSink = new QtStreamingLogSink(this, this, pendingCapacity);
    mStreamingWatcher = new QFutureWatcher<void>(this);

    if (mAnchors != nullptr)
    {
        // Anchor mutations -> scoped row repaints.
        connect(mAnchors, &AnchorManager::anchorChanged, this, &LogModel::RefreshRowsForAnchor);
        connect(mAnchors, &AnchorManager::anchorsReset, this, &LogModel::RefreshAllAnchorRows);
    }
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
        // Drop the per-batch capture alongside the table's rank cache
        // so the next session doesn't see stale demote mappings.
        mLastBatchLevelDemoteMapping.clear();
        // The cache is keyed on `LineSource*`; drop entries now so
        // recycled addresses can't masquerade as old sources.
        mCanonicalLocatorCache.clear();
        // Drop stale mismatch badges before the reset settles.
        RefreshColumnHealth();
        mFirstLevelColumnCache = LEVEL_COLUMN_UNCACHED;

        endResetModel();

        emit lineCountChanged(0);
        emit errorCountChanged(0);

        // Belt-and-braces for non-`MainWindow` callers (tests, future
        // scripted resets): `MainWindow` already clears anchors on
        // destructive opens, and `ClearAll` is idempotent.
        if (mAnchors != nullptr)
        {
            mAnchors->ClearAll();
        }
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

    // Drop cache entries for the about-to-be-released sources.
    mCanonicalLocatorCache.clear();

    mLogTable.BeginStreaming(std::move(source));
    if (reserveCount.has_value())
    {
        mLogTable.ReserveLineOffsets(*reserveCount);
    }

    // Cache the new source's canonical locator so the GUI hot
    // path stays allocation-free.
    PrewarmCanonicalLocatorCache();

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

    // Cache the new source's locator (see `BeginStreamingShared`).
    PrewarmCanonicalLocatorCache();

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
        // Resolve evicted anchors while their rows still exist.
        DropAnchorsForEvictionPrefix(dropCount);
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

    // Snapshot the pre-batch level-column index so we can restore
    // the cache below if it didn't actually change. The
    // pre-invalidation guards against re-entrant `data()` calls
    // (e.g. via a `DirectConnection` slot) reading a stale value.
    const int firstLevelColumnBefore = ComputeFirstLevelColumnIndex();
    mFirstLevelColumnCache = LEVEL_COLUMN_UNCACHED;

    // Discard the previous batch's capture so a batch without a
    // Demoted signal can't leak stale data through
    // `LastBatchLevelDemoteMappingFor`.
    mLastBatchLevelDemoteMapping.clear();

    // Snapshot pre-batch dict sizes for active enum columns; the
    // post-batch diff drives one scoped `enumColumnsChanged` per
    // affected column. For `Type::Level` columns we also capture the
    // canonical-level -> raw-bytes mapping while the dictionary is
    // still alive -- this is the only way a `Demoted` receiver can
    // translate saved canonical-name filters back into the raw
    // entries the now-string column carries.
    //
    // The NOLINT below silences `bugprone-exception-escape` on the
    // implicit move-ctor: MSVC's STL leaves `std::unordered_map`'s
    // move-ctor throwing (allocator may allocate), so the implicit
    // move-ctor of this struct also can. The struct is local --
    // emplaced into a vector and later moved-from once per column --
    // and the strong-exception failure mode (terminate via
    // bad_alloc) is the process's standard OOM behaviour, not
    // something the caller can recover from anyway.
    struct EnumSnapshotEntry // NOLINT(bugprone-exception-escape)
    {
        loglib::KeyId kid;
        int columnIndex;
        uint16_t sizeBefore;
        loglib::LogConfiguration::Type typeBefore;
        std::unordered_map<loglib::LogLevel, std::vector<std::string>> levelToRawBytes;
    };
    std::vector<EnumSnapshotEntry> enumSnapshotBefore;
    {
        const auto &columnsBefore = mLogTable.Configuration().Configuration().columns;
        const auto &keysBefore = mLogTable.Keys();
        const auto &registryBefore = mLogTable.EnumDictionaries();
        for (size_t columnIndex = 0; columnIndex < columnsBefore.size(); ++columnIndex)
        {
            const auto &column = columnsBefore[columnIndex];
            // Snapshot enum and level columns the same way: level is
            // an enumeration subtype and shares the dictionary, so
            // `Grew` / `Demoted` apply identically.
            if ((column.type != loglib::LogConfiguration::Type::Enumeration &&
                 column.type != loglib::LogConfiguration::Type::Level) ||
                column.keys.empty())
            {
                continue;
            }
            const loglib::KeyId kid = keysBefore.Find(column.keys.front());
            if (kid == loglib::INVALID_KEY_ID)
            {
                continue;
            }
            const loglib::EnumDictionary *dict = registryBefore.Find(kid);
            if (dict == nullptr)
            {
                continue;
            }
            EnumSnapshotEntry entry{
                .kid = kid,
                .columnIndex = static_cast<int>(columnIndex),
                .sizeBefore = dict->Size(),
                .typeBefore = column.type,
                .levelToRawBytes = {},
            };
            if (column.type == loglib::LogConfiguration::Type::Level)
            {
                const std::vector<loglib::LogLevel> *ranks = mLogTable.LevelRankCache(columnIndex);
                if (ranks != nullptr)
                {
                    const size_t resolveCount = std::min<size_t>(ranks->size(), dict->Size());
                    for (size_t valueId = 0; valueId < resolveCount; ++valueId)
                    {
                        const loglib::LogLevel level = (*ranks)[valueId];
                        if (level == loglib::LogLevel::Unknown)
                        {
                            continue;
                        }
                        const std::string_view bytes = dict->Resolve(static_cast<loglib::EnumValueId>(valueId));
                        entry.levelToRawBytes[level].emplace_back(bytes);
                    }
                }
            }
            enumSnapshotBefore.push_back(std::move(entry));
        }
    }

    mLogTable.AppendBatch(std::move(batch));

    // Defensive: today batches only add rows, but a future source-
    // bearing batch path must still see a warm cache.
    PrewarmCanonicalLocatorCache();

    // Restore the cache when the post-batch index didn't change;
    // otherwise leave it invalidated for lazy rebuild.
    const int firstLevelColumnAfter = ComputeFirstLevelColumnIndex();
    if (firstLevelColumnAfter == firstLevelColumnBefore)
    {
        mFirstLevelColumnCache = firstLevelColumnAfter;
    }

    // `endInsertRows` fires before `enumColumnsChanged` below, so a
    // proxy connected to `rowsInserted` walks new rows against a
    // stale predicate on a flip-batch. The follow-up
    // `enumColumnsChanged` triggers a re-walk, so steady state is
    // correct. Regression: `TestEnumFilterRebuiltAfterDemote`.
    if (rowsGrew)
    {
        endInsertRows();
    }
    if (columnsGrew)
    {
        endInsertColumns();
    }

    // `KeyId` -> source-column index lookup. Linear, but column
    // counts are tens at most, so the per-batch cost is negligible.
    const auto findColumnIndexForKey = [this](loglib::KeyId kid) -> int {
        if (kid == loglib::INVALID_KEY_ID)
        {
            return -1;
        }
        const auto &columns = mLogTable.Configuration().Configuration().columns;
        const auto &keys = mLogTable.Keys();
        for (size_t i = 0; i < columns.size(); ++i)
        {
            for (const auto &key : columns[i].keys)
            {
                if (keys.Find(key) == kid)
                {
                    return static_cast<int>(i);
                }
            }
        }
        return -1;
    };

    // Diff snapshot vs post-batch registry, one signal per (column, reason):
    //   - `Grew`: dictionary size changed.
    //   - `Demoted`: dictionary disappeared (registry erase).
    //   - `Promoted`: column flipped `Enumeration -> Level`. The two
    //     share a dictionary so only `Grew` would otherwise fire, but
    //     the filter UI needs to re-render against the level picker.
    std::vector<int> demotedColumnsThisBatch;
    if (!enumSnapshotBefore.empty())
    {
        const auto &registryAfter = mLogTable.EnumDictionaries();
        const auto &columnsAfter = mLogTable.Configuration().Configuration().columns;
        for (auto &entry : enumSnapshotBefore)
        {
            const loglib::EnumDictionary *dict = registryAfter.Find(entry.kid);
            if (dict == nullptr)
            {
                demotedColumnsThisBatch.push_back(entry.columnIndex);
                // Stash the pre-demote level mapping so the Demoted
                // receiver can rewrite canonical-name Level filters
                // into raw dictionary entries before the post-demote
                // rebuild. Plain enum demotes need no translation
                // (`levelToRawBytes` is empty there).
                if (entry.typeBefore == loglib::LogConfiguration::Type::Level && !entry.levelToRawBytes.empty())
                {
                    mLastBatchLevelDemoteMapping[entry.columnIndex] = std::move(entry.levelToRawBytes);
                }
                emit enumColumnsChanged(EnumColumnsChangeReason::Demoted, entry.columnIndex);
                continue;
            }
            const auto columnIndexSz = static_cast<size_t>(entry.columnIndex);
            const auto typeAfter =
                (columnIndexSz < columnsAfter.size()) ? columnsAfter[columnIndexSz].type : entry.typeBefore;
            if (entry.typeBefore == loglib::LogConfiguration::Type::Enumeration &&
                typeAfter == loglib::LogConfiguration::Type::Level)
            {
                // Sub-promotion supersedes `Grew`: receivers re-read
                // the column type, so one signal covers both new
                // entries and the type flip.
                emit enumColumnsChanged(EnumColumnsChangeReason::Promoted, entry.columnIndex);
            }
            else if (dict->Size() != entry.sizeBefore)
            {
                emit enumColumnsChanged(EnumColumnsChangeReason::Grew, entry.columnIndex);
            }
        }
    }
    // Same-batch `Unknown -> Enumeration -> String` isn't visible to
    // the snapshot diff (column wasn't enum at snapshot time). Use
    // `LastBatchDemotedKeys()` and de-dupe against snapshot demotes.
    for (const loglib::KeyId kid : mLogTable.LastBatchDemotedKeys())
    {
        const int columnIndex = findColumnIndexForKey(kid);
        if (columnIndex < 0)
        {
            continue;
        }
        if (std::ranges::find(demotedColumnsThisBatch, columnIndex) != demotedColumnsThisBatch.end())
        {
            continue;
        }
        demotedColumnsThisBatch.push_back(columnIndex);
        emit enumColumnsChanged(EnumColumnsChangeReason::Demoted, columnIndex);
    }

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

        // Emit `Promoted` for every back-filled column that is now an
        // enumeration. Skip columns that were already enum at
        // snapshot time: the back-fill range is a min/max over all
        // back-filled columns, so a pre-existing enum whose dict
        // only grew can fall inside it -- the `Grew` diff above
        // already covered it and a second emit would double-trigger
        // `MainWindow::UpdateFilters`.
        const auto &columns = mLogTable.Configuration().Configuration().columns;
        for (int columnIndex = firstColumn; columnIndex <= lastColumn; ++columnIndex)
        {
            if (columnIndex < 0 || std::cmp_greater_equal(columnIndex, columns.size()))
            {
                continue;
            }
            const auto colType = columns[static_cast<size_t>(columnIndex)].type;
            if (colType != loglib::LogConfiguration::Type::Enumeration &&
                colType != loglib::LogConfiguration::Type::Level)
            {
                continue;
            }
            const bool wasEnumBefore = std::ranges::any_of(enumSnapshotBefore, [columnIndex](const auto &entry) {
                return entry.columnIndex == columnIndex;
            });
            if (wasEnumBefore)
            {
                continue;
            }
            emit enumColumnsChanged(EnumColumnsChangeReason::Promoted, columnIndex);
        }
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

    // End-of-stream sweep so small streams still get enum filter
    // chips. Cancellation skips it. Emit `enumColumnsChanged` for
    // every promote / demote that lands during finalize, before
    // `streamingFinished` so the UI has a stable dictionary.
    if (!cancelled)
    {
        // Snapshot per-column types pre-finalize, plus per-Level
        // dictionary contents so a Level -> ... demote can populate
        // `mLastBatchLevelDemoteMapping` the same way the per-batch
        // path does.
        std::vector<loglib::LogConfiguration::Type> typesBefore;
        std::unordered_map<int, std::unordered_map<loglib::LogLevel, std::vector<std::string>>> levelMappingsBefore;
        {
            const auto &columnsBefore = mLogTable.Configuration().Configuration().columns;
            const auto &keysBefore = mLogTable.Keys();
            const auto &registryBefore = mLogTable.EnumDictionaries();
            typesBefore.reserve(columnsBefore.size());
            for (size_t i = 0; i < columnsBefore.size(); ++i)
            {
                const auto &column = columnsBefore[i];
                typesBefore.push_back(column.type);
                if (column.type != loglib::LogConfiguration::Type::Level || column.keys.empty())
                {
                    continue;
                }
                const loglib::KeyId kid = keysBefore.Find(column.keys.front());
                if (kid == loglib::INVALID_KEY_ID)
                {
                    continue;
                }
                const loglib::EnumDictionary *dict = registryBefore.Find(kid);
                const std::vector<loglib::LogLevel> *ranks = mLogTable.LevelRankCache(i);
                if (dict == nullptr || ranks == nullptr)
                {
                    continue;
                }
                std::unordered_map<loglib::LogLevel, std::vector<std::string>> mapping;
                const size_t resolveCount = std::min<size_t>(ranks->size(), dict->Size());
                for (size_t valueId = 0; valueId < resolveCount; ++valueId)
                {
                    const loglib::LogLevel level = (*ranks)[valueId];
                    if (level == loglib::LogLevel::Unknown)
                    {
                        continue;
                    }
                    const std::string_view bytes = dict->Resolve(static_cast<loglib::EnumValueId>(valueId));
                    mapping[level].emplace_back(bytes);
                }
                if (!mapping.empty())
                {
                    levelMappingsBefore.emplace(static_cast<int>(i), std::move(mapping));
                }
            }
        }

        // Drop per-batch demote translations; finalize repopulates.
        mLastBatchLevelDemoteMapping.clear();

        // The bool return only reports promotions; the diff loop
        // below is the source of truth for both promote and demote.
        (void)mLogTable.FinalizeAutoDetection();

        const auto &columnsAfter = mLogTable.Configuration().Configuration().columns;
        const size_t commonCount = std::min(typesBefore.size(), columnsAfter.size());
        for (size_t i = 0; i < commonCount; ++i)
        {
            const auto typeAfter = columnsAfter[i].type;
            const auto typeBefore = typesBefore[i];
            const bool isEnumLikeAfter = typeAfter == loglib::LogConfiguration::Type::Enumeration ||
                                         typeAfter == loglib::LogConfiguration::Type::Level;
            const bool wasEnumLikeBefore = typeBefore == loglib::LogConfiguration::Type::Enumeration ||
                                           typeBefore == loglib::LogConfiguration::Type::Level;
            // Fresh promotion to any enum-like type.
            if (!wasEnumLikeBefore && isEnumLikeAfter)
            {
                emit enumColumnsChanged(EnumColumnsChangeReason::Promoted, static_cast<int>(i));
                continue;
            }
            // Sub-promotion `Enumeration -> Level`: same signal so
            // the filter UI re-renders against the level picker.
            if (typeBefore == loglib::LogConfiguration::Type::Enumeration &&
                typeAfter == loglib::LogConfiguration::Type::Level)
            {
                emit enumColumnsChanged(EnumColumnsChangeReason::Promoted, static_cast<int>(i));
                continue;
            }
            // Demote out of the enum family. Stash any pre-finalize
            // level mapping so the receiver can rewrite canonical-
            // name Level filters into raw dictionary entries.
            if (wasEnumLikeBefore && !isEnumLikeAfter)
            {
                auto mappingIt = levelMappingsBefore.find(static_cast<int>(i));
                if (mappingIt != levelMappingsBefore.end())
                {
                    mLastBatchLevelDemoteMapping[static_cast<int>(i)] = std::move(mappingIt->second);
                }
                emit enumColumnsChanged(EnumColumnsChangeReason::Demoted, static_cast<int>(i));
                continue;
            }
            // Sub-demote `Level -> Enumeration`: same handling as a
            // full demote so saved Level filters get rewritten.
            if (typeBefore == loglib::LogConfiguration::Type::Level &&
                typeAfter == loglib::LogConfiguration::Type::Enumeration)
            {
                auto mappingIt = levelMappingsBefore.find(static_cast<int>(i));
                if (mappingIt != levelMappingsBefore.end())
                {
                    mLastBatchLevelDemoteMapping[static_cast<int>(i)] = std::move(mappingIt->second);
                }
                emit enumColumnsChanged(EnumColumnsChangeReason::Demoted, static_cast<int>(i));
            }
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

namespace
{
QString FormatTypeName(loglib::LogConfiguration::Type type, bool autoDetect)
{
    using Type = loglib::LogConfiguration::Type;
    QString base;
    switch (type)
    {
    case Type::Any:
        base = QStringLiteral("Any");
        break;
    case Type::String:
        base = QStringLiteral("String");
        break;
    case Type::Boolean:
        base = QStringLiteral("Boolean");
        break;
    case Type::Integer:
        base = QStringLiteral("Integer");
        break;
    case Type::Floating:
        base = QStringLiteral("Floating-point");
        break;
    case Type::Number:
        base = QStringLiteral("Number");
        break;
    case Type::Time:
        base = QStringLiteral("Time");
        break;
    case Type::Enumeration:
        base = QStringLiteral("Enumeration");
        break;
    case Type::Level:
        base = QStringLiteral("Level");
        break;
    }
    if (autoDetect && type == Type::Any)
    {
        return QStringLiteral("Any (autodetect)");
    }
    return base;
}

QString BuildHeaderTooltip(
    const loglib::LogConfiguration::Column &column,
    std::optional<loglib::LogTable::ColumnTypeHealth> health,
    const QStringList &filterTitles
)
{
    // Join with `<br/>` so missing sections don't produce blank lines.
    QStringList lines;
    if (!column.header.empty())
    {
        lines.append(QStringLiteral("<b>%1</b>").arg(QString::fromStdString(column.header).toHtmlEscaped()));
    }
    if (!column.keys.empty())
    {
        QStringList keys;
        keys.reserve(static_cast<int>(column.keys.size()));
        for (const auto &k : column.keys)
        {
            keys.append(QString::fromStdString(k).toHtmlEscaped());
        }
        lines.append(QStringLiteral("keys: %1").arg(keys.join(QStringLiteral(", "))));
    }
    lines.append(QStringLiteral("type: %1").arg(FormatTypeName(column.type, column.autoDetect)));
    QString tooltip = lines.join(QStringLiteral("<br/>"));

    // Filters section sits between metadata and the warning so the
    // warning stays the visually dominant trailing block.
    if (!filterTitles.isEmpty())
    {
        QStringList bullets;
        bullets.reserve(filterTitles.size());
        for (const auto &title : filterTitles)
        {
            bullets.append(QStringLiteral("&bull; %1").arg(title.toHtmlEscaped()));
        }
        tooltip += QStringLiteral("<br/><b>Filters:</b><br/>%1").arg(bullets.join(QStringLiteral("<br/>")));
    }

    if (health.has_value())
    {
        const size_t mismatched =
            health->presentSlots > health->matchingSlots ? health->presentSlots - health->matchingSlots : 0;
        if (mismatched > 0)
        {
            tooltip += QStringLiteral("<br/><span style=\"color:#b04040;\"><b>"
                                      "%1 of %2 values do not match the configured type.</b></span>"
                                      "<br/>Open Configuration Diagnostics to inspect or change the type.")
                           .arg(static_cast<qulonglong>(mismatched))
                           .arg(static_cast<qulonglong>(health->presentSlots));
        }
    }
    return tooltip;
}
} // namespace

QVariant LogModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || section < 0 || section >= columnCount())
    {
        return {};
    }

    if (role == Qt::DisplayRole)
    {
        return QString::fromStdString(mLogTable.GetHeader(static_cast<size_t>(section)));
    }
    if (role == Qt::ToolTipRole)
    {
        const auto &columns = mLogTable.Configuration().Configuration().columns;
        const QStringList titles = (static_cast<size_t>(section) < mColumnFilterDetails.size())
                                       ? mColumnFilterDetails[static_cast<size_t>(section)]
                                       : QStringList{};
        return BuildHeaderTooltip(columns[static_cast<size_t>(section)], ColumnHealth(section), titles);
    }
    if (role == Qt::DecorationRole)
    {
        // Warning wins over the funnel: only one decoration fits
        // in a header cell and a type mismatch is more important.
        if (auto health = ColumnHealth(section); health.has_value())
        {
            const size_t mismatched =
                health->presentSlots > health->matchingSlots ? health->presentSlots - health->matchingSlots : 0;
            if (mismatched > 0)
            {
                return QApplication::style()->standardIcon(QStyle::SP_MessageBoxWarning);
            }
        }
        if (HasFilterForColumn(section))
        {
            if (!mFunnelIconAttempted)
            {
                // Render at `PM_SmallIconSize` so the header view
                // doesn't downscale on every paint. The latch is
                // set unconditionally: a missing resource caches
                // the empty `QIcon` instead of retrying each paint.
                const QPalette appPalette = QApplication::palette();
                const QColor tint = appPalette.color(QPalette::Active, QPalette::WindowText);
                const qreal dpr = qApp->devicePixelRatio();
                int smallIconPx = 0;
                if (const QStyle *style = QApplication::style(); style != nullptr)
                {
                    smallIconPx = style->pixelMetric(QStyle::PM_SmallIconSize);
                }
                mFunnelIconCache = icon_loader::MakeThemedIcon(
                    QStringLiteral(":/icons/funnel.svg"), tint, smallIconPx, dpr
                );
                mFunnelIconAttempted = true;
            }
            return mFunnelIconCache;
        }
        return {};
    }
    return {};
}

void LogModel::NotifyColumnEdited(int columnIndex)
{
    if (columnIndex < 0 || columnIndex >= columnCount())
    {
        return;
    }
    // Type edit may have added or removed `Type::Level`.
    mFirstLevelColumnCache = LEVEL_COLUMN_UNCACHED;
    emit headerDataChanged(Qt::Horizontal, columnIndex, columnIndex);
    const int rows = rowCount();
    if (rows > 0)
    {
        emit dataChanged(
            index(0, columnIndex),
            index(rows - 1, columnIndex),
            {Qt::DisplayRole, Qt::BackgroundRole, Qt::ForegroundRole, Qt::FontRole}
        );
    }
}

void LogModel::ApplyColumnTypeEdit(int columnIndex, loglib::LogConfiguration::Type newType, bool newAutoDetect)
{
    if (columnIndex < 0 || columnIndex >= columnCount())
    {
        return;
    }
    // Snapshot the pre-edit pair so the transition classification
    // below is correct, and so `SetColumnTypePair` is the only
    // observer to see the atomic write.
    const auto &columnsBefore = mLogTable.Configuration().Configuration().columns;
    const auto previousType = columnsBefore[static_cast<size_t>(columnIndex)].type;
    const bool previousAutoDetect = columnsBefore[static_cast<size_t>(columnIndex)].autoDetect;

    if (previousType == newType && previousAutoDetect == newAutoDetect)
    {
        return;
    }

    mLogTable.Configuration().SetColumnTypePair(static_cast<size_t>(columnIndex), newType, newAutoDetect);
    mLogTable.OnUserChangedColumnType(static_cast<size_t>(columnIndex), previousType);

    // Invalidate here too (not just in `NotifyColumnEdited`) so
    // direct callers can't leave the cache stale.
    mFirstLevelColumnCache = LEVEL_COLUMN_UNCACHED;

    // Picking "Auto-detect" on already-loaded rows parks at
    // `(Any, autoDetect)`; rescan so the column actually resolves
    // instead of rendering as raw `any` forever.
    if (newType == loglib::LogConfiguration::Type::Any && newAutoDetect)
    {
        mLogTable.RescanColumnForAutoDetection(static_cast<size_t>(columnIndex));
    }

    // Read back the *effective* type -- the encode/rescan above may
    // route to a different terminal type than `newType`.
    const auto &columnsAfter = mLogTable.Configuration().Configuration().columns;
    const auto effectiveType = columnsAfter[static_cast<size_t>(columnIndex)].type;
    using Type = loglib::LogConfiguration::Type;
    const bool wasEnumLike = previousType == Type::Enumeration || previousType == Type::Level;
    const bool isEnumLike = effectiveType == Type::Enumeration || effectiveType == Type::Level;
    // Promote: entering the enum family, or sub-promote
    // `Enumeration -> Level`. Demote: leaving it, or sub-demote
    // `Level -> Enumeration` (same rebuild gate as a real demote).
    const bool isPromote =
        (!wasEnumLike && isEnumLike) || (previousType == Type::Enumeration && effectiveType == Type::Level);
    const bool isDemote =
        (wasEnumLike && !isEnumLike) || (previousType == Type::Level && effectiveType == Type::Enumeration);
    if (isPromote)
    {
        emit enumColumnsChanged(EnumColumnsChangeReason::Promoted, columnIndex);
    }
    else if (isDemote)
    {
        emit enumColumnsChanged(EnumColumnsChangeReason::Demoted, columnIndex);
    }

    // Encode / back-fill changed slot tags -- health cache is stale.
    RefreshColumnHealth();
}

std::optional<loglib::LogTable::ColumnTypeHealth> LogModel::ColumnHealth(int section) const
{
    if (section < 0 || static_cast<size_t>(section) >= mColumnHealth.size())
    {
        return std::nullopt;
    }
    return mColumnHealth[static_cast<size_t>(section)];
}

void LogModel::SetColumnFilterDetails(std::vector<QStringList> perColumnTitles)
{
    const int cols = columnCount();
    if (cols <= 0)
    {
        // Structural reset in flight: drop the cache. No emit -
        // the surrounding `beginResetModel`/`endResetModel` already
        // invalidates the header on the view side.
        if (mColumnFilterDetails.empty())
        {
            return;
        }
        mColumnFilterDetails.clear();
        return;
    }

    // Normalise input length to current column count: trim extras,
    // pad with empty lists so cleared columns get reset.
    perColumnTitles.resize(static_cast<size_t>(cols));
    if (mColumnFilterDetails.size() != perColumnTitles.size())
    {
        mColumnFilterDetails.resize(perColumnTitles.size());
    }

    int firstChanged = -1;
    int lastChanged = -1;
    for (int i = 0; i < cols; ++i)
    {
        if (mColumnFilterDetails[static_cast<size_t>(i)] != perColumnTitles[static_cast<size_t>(i)])
        {
            if (firstChanged < 0)
            {
                firstChanged = i;
            }
            lastChanged = i;
        }
    }
    if (firstChanged < 0)
    {
        // Idempotent no-op: suppress the emit so row-storm signals
        // don't trigger redundant header repaints.
        return;
    }
    mColumnFilterDetails = std::move(perColumnTitles);
    emit headerDataChanged(Qt::Horizontal, firstChanged, lastChanged);
}

bool LogModel::HasFilterForColumn(int section) const noexcept
{
    if (section < 0 || static_cast<size_t>(section) >= mColumnFilterDetails.size())
    {
        return false;
    }
    return !mColumnFilterDetails[static_cast<size_t>(section)].isEmpty();
}

void LogModel::RefreshHeaderIcons()
{
    mFunnelIconCache = QIcon{};
    // Reset the latch so the next decoration query re-renders the
    // funnel against the new palette / DPR.
    mFunnelIconAttempted = false;
    // Re-emit only the contiguous range of filtered columns; the
    // rest don't show the funnel and need no repaint.
    int firstWith = -1;
    int lastWith = -1;
    for (size_t i = 0; i < mColumnFilterDetails.size(); ++i)
    {
        if (!mColumnFilterDetails[i].isEmpty())
        {
            if (firstWith < 0)
            {
                firstWith = static_cast<int>(i);
            }
            lastWith = static_cast<int>(i);
        }
    }
    if (firstWith < 0)
    {
        return;
    }
    emit headerDataChanged(Qt::Horizontal, firstWith, lastWith);
}

void LogModel::RefreshColumnHealth()
{
    std::vector<loglib::LogTable::ColumnTypeHealth> next;
    const size_t cols = mLogTable.ColumnCount();
    next.reserve(cols);
    for (size_t i = 0; i < cols; ++i)
    {
        next.push_back(mLogTable.ComputeColumnTypeHealth(i));
    }

    if (next == mColumnHealth)
    {
        return;
    }
    mColumnHealth = std::move(next);
    if (cols > 0)
    {
        emit headerDataChanged(Qt::Horizontal, 0, static_cast<int>(cols) - 1);
    }
    emit columnHealthChanged();
}

int LogModel::ComputeFirstLevelColumnIndex() const noexcept
{
    const auto &columns = Configuration().columns;
    for (size_t i = 0; i < columns.size(); ++i)
    {
        if (columns[i].type == loglib::LogConfiguration::Type::Level)
        {
            return static_cast<int>(i);
        }
    }
    return LEVEL_COLUMN_NONE;
}

std::optional<AnchorManager::Key> LogModel::AnchorKeyForRow(int row) const noexcept
{
    if (row < 0)
    {
        return std::nullopt;
    }
    const auto unsignedRow = static_cast<std::size_t>(row);
    const std::vector<loglib::LogLine> &lines = mLogTable.Data().Lines();
    if (unsignedRow >= lines.size())
    {
        return std::nullopt;
    }
    const loglib::LogLine &line = lines[unsignedRow];
    const loglib::LineSource *source = line.Source();
    // Qt's paint stack invokes `data()` and can't unwind through
    // `bad_alloc`. Swallow alloc failures and report no anchor.
    try
    {
        AnchorManager::Key key;
        key.lineId = static_cast<uint64_t>(line.LineId());
        if (source != nullptr && !source->Path().empty())
        {
            // Lookup only; `PrewarmCanonicalLocatorCache` fills the
            // cache on every source mutation. Misses produce an
            // empty locator (anchor simply won't match).
            const auto it = mCanonicalLocatorCache.find(source);
            if (it != mCanonicalLocatorCache.end())
            {
                key.locator = it->second;
            }
        }
        return key;
    }
    catch (...)
    {
        return std::nullopt;
    }
}

int LogModel::SourceRowForAnchorKey(const AnchorManager::Key &key) const noexcept
{
    const std::vector<loglib::LogLine> &lines = mLogTable.Data().Lines();
    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        // Cheap `lineId` filter first; multi-file sessions still
        // need the locator check below because ids can collide
        // across sources.
        if (static_cast<uint64_t>(lines[i].LineId()) != key.lineId)
        {
            continue;
        }
        const auto rowKey = AnchorKeyForRow(static_cast<int>(i));
        if (rowKey.has_value() && *rowKey == key)
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void LogModel::PrewarmCanonicalLocatorCache()
{
    // Idempotent: existing entries are skipped, only new sources
    // pay the canonicalisation. The table never relocates a source
    // pointer, so `LineSource *` is a stable key.
    for (const auto &sourcePtr : mLogTable.Data().Sources())
    {
        const loglib::LineSource *source = sourcePtr.get();
        if (source == nullptr || source->Path().empty())
        {
            continue;
        }
        if (mCanonicalLocatorCache.contains(source))
        {
            continue;
        }
        std::string canonical = logapp::CanonicalLocator(QString::fromStdString(source->Path().string())).toStdString();
        mCanonicalLocatorCache.emplace(source, std::move(canonical));
    }
}

void LogModel::RefreshRowsForAnchor(const AnchorManager::Key &key)
{
    const int cols = columnCount();
    if (cols <= 0)
    {
        return;
    }
    // Anchor toggles are user-paced; a linear scan beats keeping a
    // reverse map in sync across batch appends and FIFO eviction.
    // Cheap `lineId` filter first to skip the cache lookup on
    // misses (the bulk of the iterations).
    const std::vector<loglib::LogLine> &lines = mLogTable.Data().Lines();
    for (size_t i = 0; i < lines.size(); ++i)
    {
        if (static_cast<uint64_t>(lines[i].LineId()) != key.lineId)
        {
            continue;
        }
        const auto rowKey = AnchorKeyForRow(static_cast<int>(i));
        if (!rowKey.has_value() || *rowKey != key)
        {
            continue;
        }
        const QModelIndex topLeft = index(static_cast<int>(i), 0);
        const QModelIndex bottomRight = index(static_cast<int>(i), cols - 1);
        emit dataChanged(topLeft, bottomRight, {Qt::BackgroundRole, Qt::ForegroundRole});
    }
}

void LogModel::RefreshAllAnchorRows()
{
    const int rows = rowCount();
    const int cols = columnCount();
    if (rows <= 0 || cols <= 0)
    {
        return;
    }
    emit dataChanged(index(0, 0), index(rows - 1, cols - 1), {Qt::BackgroundRole, Qt::ForegroundRole});
}

void LogModel::RefreshAllRowStyles()
{
    const int rows = rowCount();
    const int cols = columnCount();
    if (rows <= 0 || cols <= 0)
    {
        return;
    }
    // FontRole rides along: themes can bold/italicise per level, so
    // a flip can change font weight on level-styled rows.
    emit dataChanged(index(0, 0), index(rows - 1, cols - 1), {Qt::BackgroundRole, Qt::ForegroundRole, Qt::FontRole});
}

bool LogModel::IsStyleOnlyRoleChange(const QList<int> &roles) noexcept
{
    if (roles.isEmpty())
    {
        // Qt's "I don't know what changed" sentinel; refresh conservatively.
        return false;
    }
    return std::ranges::all_of(roles, [](int role) {
        return role == Qt::BackgroundRole || role == Qt::ForegroundRole || role == Qt::FontRole ||
               role == Qt::DecorationRole;
    });
}

void LogModel::DropAnchorsForEvictionPrefix(int dropCount)
{
    if (mAnchors == nullptr || dropCount <= 0 || mAnchors->Empty())
    {
        return;
    }
    // Collect first, mutate second: `RemoveAnchors` re-enters
    // `RefreshAllAnchorRows`, so we must finish walking the rows
    // before they're evicted.
    std::vector<AnchorManager::Key> evictedKeys;
    evictedKeys.reserve(static_cast<std::size_t>(dropCount));
    for (int row = 0; row < dropCount; ++row)
    {
        if (auto key = AnchorKeyForRow(row); key.has_value())
        {
            evictedKeys.push_back(std::move(*key));
        }
    }
    if (!evictedKeys.empty())
    {
        // Bulk path collapses to a single `anchorsReset`.
        mAnchors->RemoveAnchors(evictedKeys);
    }
}

std::optional<loglib::LogLevel> LogModel::LevelForRow(int row) const noexcept
{
    if (mFirstLevelColumnCache == LEVEL_COLUMN_UNCACHED)
    {
        mFirstLevelColumnCache = ComputeFirstLevelColumnIndex();
    }
    if (mFirstLevelColumnCache < 0 || row < 0)
    {
        return std::nullopt;
    }
    return mLogTable.GetLevelForRow(static_cast<size_t>(row), static_cast<size_t>(mFirstLevelColumnCache));
}

QVariant LogModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= rowCount() || index.column() >= columnCount())
    {
        return {};
    }

    // Style roles (Background / Foreground / Font) each resolve
    // the row's level via `LevelForRow`. `Qt::FontRole` is gated
    // on `HasAnyFontStyle()` so themes that style no level skip
    // the resolve entirely.
    switch (role)
    {
    case Qt::DisplayRole:
        return ConvertToSingleLineCompactQString(
            mLogTable.GetFormattedValue(static_cast<size_t>(index.row()), static_cast<size_t>(index.column()))
        );

    case Qt::BackgroundRole:
    {
        if (mTheme == nullptr)
        {
            return {};
        }
        // Anchor overlay wins over the level brush. `Empty()` keeps
        // the common no-anchor case cheap.
        if (mAnchors != nullptr && !mAnchors->Empty())
        {
            const auto key = AnchorKeyForRow(index.row());
            if (key.has_value())
            {
                if (const auto colorIndex = mAnchors->ColorFor(*key); colorIndex.has_value())
                {
                    const QBrush brush = mTheme->AnchorBrushFor(*colorIndex, Qt::BackgroundRole);
                    if (brush.style() != Qt::NoBrush)
                    {
                        return QVariant(brush);
                    }
                }
            }
        }
        const std::optional<loglib::LogLevel> level = LevelForRow(index.row());
        if (!level.has_value())
        {
            return {};
        }
        const QBrush brush = mTheme->BackgroundFor(*level);
        return brush.style() != Qt::NoBrush ? QVariant(brush) : QVariant{};
    }

    case Qt::ForegroundRole:
    {
        if (mTheme == nullptr)
        {
            return {};
        }
        // Mirrors the BackgroundRole anchor overlay.
        if (mAnchors != nullptr && !mAnchors->Empty())
        {
            const auto key = AnchorKeyForRow(index.row());
            if (key.has_value())
            {
                if (const auto colorIndex = mAnchors->ColorFor(*key); colorIndex.has_value())
                {
                    const QBrush brush = mTheme->AnchorBrushFor(*colorIndex, Qt::ForegroundRole);
                    if (brush.style() != Qt::NoBrush)
                    {
                        return QVariant(brush);
                    }
                }
            }
        }
        const std::optional<loglib::LogLevel> level = LevelForRow(index.row());
        if (!level.has_value())
        {
            return {};
        }
        const QBrush brush = mTheme->ForegroundFor(*level);
        return brush.style() != Qt::NoBrush ? QVariant(brush) : QVariant{};
    }

    case Qt::FontRole:
    {
        // Skip the per-cell resolve when no level is styled.
        // Per-level check is still needed below for themes that
        // style only some levels.
        if (mTheme == nullptr || !mTheme->HasAnyFontStyle())
        {
            return {};
        }
        const std::optional<loglib::LogLevel> level = LevelForRow(index.row());
        if (!level.has_value() || !mTheme->HasFontStyle(*level))
        {
            return {};
        }
        return mTheme->FontFor(*level);
    }

    case LogModelItemDataRole::SortRole:
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

    case LogModelItemDataRole::InsertionOrderRole:
        return {index.row()};

    case LogModelItemDataRole::CopyLine:
    {
        // `LineSource *` distinguishes mmap arena vs. owned bytes.
        const auto row = static_cast<size_t>(index.row());
        const auto &line = mLogTable.Data().Lines()[row];
        const loglib::LineSource *source = line.Source();
        const std::string raw = source != nullptr ? source->RawLine(line.LineId()) : std::string{};
        return {QString::fromStdString(raw)};
    }

    case LogModelItemDataRole::EnumValueRole:
    {
        // `qint32` for `DictRef` slots; invalid for monostate /
        // unpromoted slots. Exposed for tests / external readers; the
        // filter predicate path queries `LogTable` directly.
        const auto enumId =
            mLogTable.GetEnumValueId(static_cast<size_t>(index.row()), static_cast<size_t>(index.column()));
        if (!enumId.has_value())
        {
            return {};
        }
        return QVariant::fromValue<qint32>(static_cast<qint32>(*enumId));
    }

    default:
        return {};
    }
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
            // Resolve evicted anchors before the rows go away.
            DropAnchorsForEvictionPrefix(static_cast<int>(dropCount));
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

QString LogModel::ConvertToSingleLineCompactQString(std::string_view bytes)
{
    QString qString = QString::fromUtf8(bytes.data(), static_cast<qsizetype>(bytes.size()));
    qString.replace(QLatin1Char('\n'), QLatin1Char(' '));
    qString.replace(QLatin1Char('\r'), QLatin1Char(' '));
    return qString.simplified();
}

bool LogModel::IsSingleLineAsciiTrim(std::string_view bytes) noexcept
{
    // `ConvertToSingleLineCompactQString` (1) decodes UTF-8,
    // (2) replaces `\n` / `\r` with space, (3) collapses internal
    // whitespace runs, (4) trims leading + trailing whitespace. We
    // must reject any input the conversion would actually touch;
    // everything else is byte-equal to the conversion result.
    constexpr unsigned char ASCII_FIRST_NON_CONTROL = 0x20; // space; lower bytes are control.
    constexpr unsigned char ASCII_HIGH_BIT = 0x80;          // first non-ASCII byte.
    constexpr unsigned char ASCII_DEL = 0x7F;               // DEL is a control byte despite living at 0x7F.

    if (bytes.empty())
    {
        return true;
    }
    const auto first = static_cast<unsigned char>(bytes.front());
    if (first <= ASCII_FIRST_NON_CONTROL)
    {
        // Leading whitespace / control byte -- conversion would trim
        // or replace it.
        return false;
    }
    const auto last = static_cast<unsigned char>(bytes.back());
    if (last <= ASCII_FIRST_NON_CONTROL)
    {
        return false;
    }
    bool prevSpace = false;
    for (const char ch : bytes)
    {
        const auto c = static_cast<unsigned char>(ch);
        if (c >= ASCII_HIGH_BIT)
        {
            // Non-ASCII byte: `QString::fromUtf8` may decode to a
            // different code-unit count, so byte compare would diverge.
            return false;
        }
        if (c == ' ')
        {
            if (prevSpace)
            {
                // Two-or-more-space run; `simplified()` collapses it.
                return false;
            }
            prevSpace = true;
            continue;
        }
        if (c < ASCII_FIRST_NON_CONTROL || c == ASCII_DEL)
        {
            // ASCII control byte (`\n`, `\r`, `\t`, `\v`, `\f`, DEL, ...).
            // `simplified()` treats them as whitespace; byte compare
            // would not.
            return false;
        }
        prevSpace = false;
    }
    return true;
}

void LogModel::NotifyConfigurationReplaced()
{
    // `LogConfigurationManager::Load` rewrites the configuration
    // without emitting any model signal. Re-sync the per-column
    // caches before the reset so mid-reset queries see consistent
    // state. The row store is unchanged here.
    beginResetModel();
    mLogTable.OnConfigurationReloaded();
    mFirstLevelColumnCache = LEVEL_COLUMN_UNCACHED;
    endResetModel();
}

const std::unordered_map<loglib::LogLevel, std::vector<std::string>> *LogModel::LastBatchLevelDemoteMappingFor(
    int columnIndex
) const noexcept
{
    const auto it = mLastBatchLevelDemoteMapping.find(columnIndex);
    if (it == mLastBatchLevelDemoteMapping.end())
    {
        return nullptr;
    }
    return &it->second;
}

bool LogModel::MoveColumn(int srcIndex, int destIndex)
{
    if (srcIndex == destIndex)
    {
        return false;
    }
    const int cols = columnCount();
    // `destIndex` is the absolute final position (same as
    // `LogTable::MoveColumn`).
    if (srcIndex < 0 || srcIndex >= cols || destIndex < 0 || destIndex >= cols)
    {
        return false;
    }
    // Translate to Qt's "insert before" `destinationChild`:
    // rightward moves (`srcIndex < destIndex`) land at `destIndex - 1`
    // unless we shift by one; leftward moves agree with `destIndex`.
    const int qtDestinationChild = (srcIndex < destIndex) ? destIndex + 1 : destIndex;
    if (!beginMoveColumns(QModelIndex(), srcIndex, srcIndex, QModelIndex(), qtDestinationChild))
    {
        return false;
    }
    // `LogTable::MoveColumn` rotates `columns` and remaps every
    // `LogFilter::row` via the configuration manager.
    mLogTable.MoveColumn(static_cast<size_t>(srcIndex), static_cast<size_t>(destIndex));
    mFirstLevelColumnCache = LEVEL_COLUMN_UNCACHED;
    endMoveColumns();
    return true;
}
