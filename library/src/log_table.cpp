#include "loglib/log_table.hpp"

#include "loglib/log_processing.hpp"

#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace loglib
{

LogTable::LogTable(LogData data, LogConfigurationManager configuration)
    : mData(std::move(data)), mConfiguration(std::move(configuration))
{
    RefreshColumnKeyIds();
}

void LogTable::Update(LogData &&data)
{
    Configuration().Update(data);
    if (!data.TimestampsAlreadyParsed())
    {
        ParseTimestamps(data, Configuration().Configuration());
    }
    Data().Merge(std::move(data));
    RefreshColumnKeyIds();
}

void LogTable::BeginStreaming(std::unique_ptr<LogFile> file)
{
    // Streaming entry point. Replaces any prior data with a fresh LogData backed by @p file
    // (so subsequent AppendBatch calls can splice batches into a single LogData rather than
    // accumulating one LogData per batch and merging at the end). Snapshots the time-column
    // KeyId set against the *current* configuration — this is the same configuration that
    // the Stage B parser is about to be handed (LogModel wraps it in a shared_ptr<const>),
    // so any time column that lands in mStageBSnapshotTimeKeys here is guaranteed to have
    // been parsed by Stage B for every line the parser produces. Anything that appears
    // *after* this snapshot (e.g. an auto-promoted new key) is what AppendBatch back-fills
    // from req. 4.1.13b.
    mLastBackfillRange.reset();

    if (file)
    {
        std::vector<LogLine> noLines;
        LogData fresh(std::move(file), std::move(noLines), KeyIndex{});
        // PRD §4.2a / parser-perf task 3.6: Stage B now actually promotes Type::time column
        // values to TimeStamp inline (see `JsonParser::ParseStreaming`'s `timeColumns`
        // pre-resolution + `ParseBatchBody`'s per-line `ParseTimestampLine` call), so this
        // flag is truthful from the moment the streaming pipeline starts. Time columns whose
        // KeyIds first appear *after* the Stage B snapshot (i.e. auto-promoted mid-stream
        // from a key surfaced by a later batch) are still back-filled on the GUI thread by
        // `LogTable::AppendBatch` step 4 against `mStageBSnapshotTimeKeys`, but those are
        // *additional* columns layered on top of an already-parsed line set rather than the
        // load-bearing lie the flag used to be (PRD A.8 reference).
        fresh.MarkTimestampsParsed();
        mData = std::move(fresh);
    }
    else
    {
        // File-less initialisation path used by fixture tests that hand-craft batches.
        mData = LogData{};
        mData.MarkTimestampsParsed();
    }

    RefreshColumnKeyIds();
    RefreshSnapshotTimeKeys();
}

void LogTable::AppendBatch(StreamedBatch batch)
{
    mLastBackfillRange.reset();

    // Step 1: extend the configuration if the batch surfaced new keys. Append-only — never
    // reorder — so already-known column indices stay put for any consumer that has already
    // observed them (PRD req. 4.1.13).
    if (!batch.newKeys.empty())
    {
        mConfiguration.AppendKeys(batch.newKeys);
    }

    // Step 2: splice the lines + line-offset table into the owned LogData. The lines are
    // already bound to the canonical KeyIndex (the parser borrows it via
    // StreamingLogSink::Keys), but LogData::AppendBatch defensively rebinds in case a
    // hand-crafted test feeds in lines bound to a different KeyIndex.
    if (!batch.lines.empty() || !batch.localLineOffsets.empty())
    {
        mData.AppendBatch(std::move(batch.lines), std::move(batch.localLineOffsets));
    }

    // Step 3: refresh the column → KeyId cache only when this batch surfaced new keys.
    // Without new keys the existing cache is still correct because `KeyIndex` assigns dense,
    // monotonically-increasing KeyIds (PRD §4.2): a key that resolved last batch will resolve
    // to the same id this batch, and a key that resolved to `kInvalidKeyId` last batch can
    // only flip to a valid id by being added to the configuration here — which, by definition,
    // means it appeared in `batch.newKeys`. The incremental path further restricts the patch
    // to columns whose `keys` vector overlaps with `batch.newKeys`, leaving the rest of the
    // cache untouched (PRD §4.8.2). For a 100-column / 1 000-batch streaming parse with zero
    // new keys after batch 1 this saves ~99 000 redundant `KeyIndex::Find` calls on the GUI
    // thread.
    if (!batch.newKeys.empty())
    {
        RefreshColumnKeyIdsForKeys(batch.newKeys);
    }

    // Step 4: walk the configuration once more for time columns whose KeyId set is not a
    // subset of the Stage-B snapshot. Each such column is one that Stage B did not parse
    // (because the column did not exist in the snapshot configuration when the parse began),
    // so we back-fill it over *every* row currently in mData.Lines() — already-appended +
    // just-appended — exactly once. Append the back-filled column's keys into the snapshot
    // so subsequent batches see those keys as already-handled.
    const auto &columns = mConfiguration.Configuration().columns;
    std::optional<size_t> firstBackfilled;
    std::optional<size_t> lastBackfilled;
    for (size_t columnIndex = 0; columnIndex < columns.size(); ++columnIndex)
    {
        const auto &column = columns[columnIndex];
        if (column.type != LogConfiguration::Type::time)
        {
            continue;
        }

        bool needsBackfill = false;
        std::vector<KeyId> columnKeyIds;
        columnKeyIds.reserve(column.keys.size());
        for (const std::string &key : column.keys)
        {
            const KeyId id = mData.Keys().Find(key);
            columnKeyIds.push_back(id);
            if (id != kInvalidKeyId && !mStageBSnapshotTimeKeys.contains(id))
            {
                needsBackfill = true;
            }
        }
        if (!needsBackfill)
        {
            continue;
        }

        BackfillTimestampColumn(column, mData.Lines());

        for (const KeyId id : columnKeyIds)
        {
            if (id != kInvalidKeyId)
            {
                mStageBSnapshotTimeKeys.insert(id);
            }
        }

        if (!firstBackfilled.has_value())
        {
            firstBackfilled = columnIndex;
        }
        lastBackfilled = columnIndex;
    }

    if (firstBackfilled.has_value())
    {
        mLastBackfillRange = std::make_pair(*firstBackfilled, *lastBackfilled);
    }
}

const std::optional<std::pair<size_t, size_t>> &LogTable::LastBackfillRange() const
{
    return mLastBackfillRange;
}

std::string LogTable::GetHeader(size_t column) const
{
    return mConfiguration.Configuration().columns[column].header;
}

size_t LogTable::ColumnCount() const
{
    return mConfiguration.Configuration().columns.size();
}

LogValue LogTable::GetValue(size_t row, size_t column) const
{
    if (column >= mColumnKeyIds.size())
    {
        return std::monostate{};
    }
    const auto &line = mData.Lines()[row];
    for (const KeyId id : mColumnKeyIds[column])
    {
        if (id == kInvalidKeyId)
        {
            continue;
        }
        LogValue value = line.GetValue(id);
        if (!std::holds_alternative<std::monostate>(value))
        {
            return value;
        }
    }
    return std::monostate{};
}

std::string LogTable::GetFormattedValue(size_t row, size_t column) const
{
    if (column >= mColumnKeyIds.size())
    {
        return "";
    }
    const auto &line = mData.Lines()[row];
    for (const KeyId id : mColumnKeyIds[column])
    {
        if (id == kInvalidKeyId)
        {
            continue;
        }
        LogValue value = line.GetValue(id);
        if (!std::holds_alternative<std::monostate>(value))
        {
            return FormatLogValue(mConfiguration.Configuration().columns.at(column).printFormat, value);
        }
    }
    return "";
}

size_t LogTable::RowCount() const
{
    return mData.Lines().size();
}

const LogData &LogTable::Data() const
{
    return mData;
}

LogData &LogTable::Data()
{
    return mData;
}

const LogConfigurationManager &LogTable::Configuration() const
{
    return mConfiguration;
}

LogConfigurationManager &LogTable::Configuration()
{
    return mConfiguration;
}

void LogTable::RefreshColumnKeyIds()
{
    const auto &columns = mConfiguration.Configuration().columns;
    mColumnKeyIds.clear();
    mColumnKeyIds.reserve(columns.size());
    for (const auto &column : columns)
    {
        std::vector<KeyId> ids;
        ids.reserve(column.keys.size());
        for (const auto &key : column.keys)
        {
            ids.push_back(mData.Keys().Find(key));
        }
        mColumnKeyIds.push_back(std::move(ids));
    }
}

void LogTable::RefreshColumnKeyIdsForKeys(const std::vector<std::string> &newKeys)
{
    // PRD §4.8.2 / parser-perf task 9.3: incremental column → KeyId refresh. Two design
    // notes worth keeping in this function:
    //
    // 1. We iterate `mConfiguration.Configuration().columns` *after* `AppendBatch` has
    //    already extended the configuration via `AppendKeys`, so any column added by the
    //    just-arrived batch is now visible at the tail of the `columns` vector. The
    //    `mColumnKeyIds` cache may therefore be shorter than `columns` — in that case we
    //    grow it with empty inner vectors, then fall through to the lookup loop which
    //    populates them in place. This preserves the post-condition that
    //    `mColumnKeyIds.size() == columns.size()` after every successful return.
    //
    // 2. The `string_view` set is built once per batch from `newKeys` (typically a small
    //    handful of strings, often empty after batch 1 in steady state). Per-column we
    //    walk `column.keys` and only rebuild the inner vector if any of those keys is in
    //    the newKeys set. Untouched columns keep their cached KeyIds — `KeyIndex` is
    //    monotonic so a previously-resolved id is still valid this batch.
    if (newKeys.empty())
    {
        return;
    }

    std::unordered_set<std::string_view> newKeySet;
    newKeySet.reserve(newKeys.size());
    for (const std::string &key : newKeys)
    {
        newKeySet.emplace(key);
    }

    const auto &columns = mConfiguration.Configuration().columns;
    if (mColumnKeyIds.size() < columns.size())
    {
        mColumnKeyIds.resize(columns.size());
    }

    for (size_t columnIndex = 0; columnIndex < columns.size(); ++columnIndex)
    {
        const auto &column = columns[columnIndex];

        bool affected = mColumnKeyIds[columnIndex].size() != column.keys.size();
        if (!affected)
        {
            for (const std::string &key : column.keys)
            {
                if (newKeySet.find(std::string_view(key)) != newKeySet.end())
                {
                    affected = true;
                    break;
                }
            }
        }
        if (!affected)
        {
            continue;
        }

        std::vector<KeyId> ids;
        ids.reserve(column.keys.size());
        for (const std::string &key : column.keys)
        {
            ids.push_back(mData.Keys().Find(key));
        }
        mColumnKeyIds[columnIndex] = std::move(ids);
    }
}

void LogTable::RefreshSnapshotTimeKeys()
{
    // Capture every KeyId currently referenced by a Type::time column. Anything Stage B's
    // configuration snapshot can already see at this point will land in this set; anything
    // that arrives later (auto-promoted from a freshly-discovered key in a later batch) will
    // be missing and AppendBatch will trigger the back-fill from req. 4.1.13b.
    mStageBSnapshotTimeKeys.clear();
    const auto &columns = mConfiguration.Configuration().columns;
    for (const auto &column : columns)
    {
        if (column.type != LogConfiguration::Type::time)
        {
            continue;
        }
        for (const std::string &key : column.keys)
        {
            const KeyId id = mData.Keys().Find(key);
            if (id != kInvalidKeyId)
            {
                mStageBSnapshotTimeKeys.insert(id);
            }
        }
    }
}

std::string LogTable::FormatLogValue(const std::string &format, const LogValue &value)
{
    return std::visit(
        [&format](const auto &arg) -> std::string {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, std::string>)
            {
                return arg;
            }
            else if constexpr (std::is_same_v<T, std::string_view>)
            {
                return std::string(arg);
            }
            else if constexpr (std::is_same_v<T, int64_t>)
            {
                return fmt::vformat(format, fmt::make_format_args(arg));
            }
            else if constexpr (std::is_same_v<T, uint64_t>)
            {
                return fmt::vformat(format, fmt::make_format_args(arg));
            }
            else if constexpr (std::is_same_v<T, double>)
            {
                return fmt::vformat(format, fmt::make_format_args(arg));
            }
            else if constexpr (std::is_same_v<T, bool>)
            {
                return fmt::vformat(format, fmt::make_format_args(arg));
            }
            else if constexpr (std::is_same_v<T, TimeStamp>)
            {
                const date::zoned_time local_time{CurrentZone(), std::chrono::round<std::chrono::milliseconds>(arg)};
                return date::format(format, local_time);
            }
            else if constexpr (std::is_same_v<T, std::monostate>)
            {
                return std::string();
            }
            else
            {
                static_assert(std::is_same_v<T, void>, "non-exhaustive visitor!");
            }
        },
        value
    );
}

} // namespace loglib
