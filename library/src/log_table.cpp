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
    // and snapshots the time-column KeyId set against the current configuration (which is
    // the one the streaming parser will see). KeyIds present at this point are guaranteed
    // to have been parsed inline by the parser; KeyIds that appear later (auto-promoted
    // from a later batch's new keys) are back-filled by `AppendBatch`.
    mLastBackfillRange.reset();

    if (file)
    {
        std::vector<LogLine> noLines;
        LogData fresh(std::move(file), std::move(noLines), KeyIndex{});
        // The streaming parser promotes Type::time values to TimeStamp inline, so this
        // flag is truthful from the moment the pipeline starts. Time columns first seen
        // *after* this snapshot are still back-filled on the GUI thread by `AppendBatch`.
        fresh.MarkTimestampsParsed();
        mData = std::move(fresh);
    }
    else
    {
        // File-less init for fixture tests that hand-craft batches.
        mData = LogData{};
        mData.MarkTimestampsParsed();
    }

    RefreshColumnKeyIds();
    RefreshSnapshotTimeKeys();
}

void LogTable::AppendBatch(StreamedBatch batch)
{
    mLastBackfillRange.reset();

    // Step 1: extend the configuration if the batch surfaced new keys. Append-only,
    // so already-known column indices stay put for any consumer that has observed them.
    if (!batch.newKeys.empty())
    {
        mConfiguration.AppendKeys(batch.newKeys);
    }

    // Step 2: splice lines + line offsets into the owned LogData. Lines are already
    // bound to the canonical KeyIndex (borrowed via `StreamingLogSink::Keys`), but
    // `AppendBatch` defensively rebinds for hand-crafted test fixtures.
    if (!batch.lines.empty() || !batch.localLineOffsets.empty())
    {
        mData.AppendBatch(std::move(batch.lines), std::move(batch.localLineOffsets));
    }

    // Step 3: refresh the column → KeyId cache only when new keys arrived. KeyIds are
    // dense and monotonic, so a key that resolved last batch still resolves the same
    // way; a `kInvalidKeyId` can only flip to valid via a `newKeys` addition.
    if (!batch.newKeys.empty())
    {
        RefreshColumnKeyIdsForKeys(batch.newKeys);
    }

    // Step 4: back-fill time columns whose KeyId set is not a subset of the snapshot.
    // Each such column was added after streaming began, so we back-fill it across every
    // row in `mData.Lines()` exactly once and append its keys to the snapshot.
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
    // Incremental column → KeyId cache refresh. Two design notes:
    //   1. We run *after* `AppendBatch` has extended the configuration via `AppendKeys`,
    //      so any newly-added column appears at the tail of `columns`. If `mColumnKeyIds`
    //      is shorter than `columns` we grow it with empty inner vectors and the loop
    //      below populates them, preserving the size invariant on return.
    //   2. The `string_view` set is built once from `newKeys` (typically empty after
    //      batch 1). Columns whose `keys` don't overlap with `newKeySet` keep their
    //      cached entries — KeyIds are monotonic so resolved ids stay resolved.
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
    // Capture every KeyId currently referenced by a Type::time column. Keys present at
    // this moment have been parsed inline by the streaming parser; keys that arrive
    // later are absent here and `AppendBatch` will trigger their back-fill.
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
