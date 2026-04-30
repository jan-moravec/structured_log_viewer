#include "loglib/log_table.hpp"

#include "loglib/log_processing.hpp"

#include <date/date.h>
#include <date/tz.h>
#include <fmt/format.h>

#include <span>
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
    mConfiguration.Update(data);
    if (!data.TimestampsAlreadyParsed())
    {
        ParseTimestamps(data, mConfiguration.Configuration());
    }
    mData.Merge(std::move(data));
    RefreshColumnKeyIds();
}

void LogTable::Reset()
{
    // Wipe per-parse state but preserve `mConfiguration`. Both row vectors
    // (file rows and stream rows) clear together so RowCount() returns 0.
    mData = LogData{};
    mStageBSnapshotTimeKeys.clear();
    mPostSnapshotTimeKeys.clear();
    mLastBackfillRange.reset();
    RefreshColumnKeyIds();
}

void LogTable::BeginStreaming(std::unique_ptr<LogFile> file)
{
    mLastBackfillRange.reset();

    if (file)
    {
        std::vector<LogLine> noLines;
        LogData fresh(std::move(file), std::move(noLines), KeyIndex{});
        // Stage B promotes Type::time inline; later-discovered time columns
        // are back-filled in `AppendBatch`.
        fresh.MarkTimestampsParsed();
        mData = std::move(fresh);
    }
    else
    {
        mData = LogData{};
        mData.MarkTimestampsParsed();
    }

    // Order matters: snapshot inserts the time-column keys before the
    // KeyId resolution runs.
    RefreshSnapshotTimeKeys();
    RefreshColumnKeyIds();
}

void LogTable::AppendBatch(StreamedBatch batch)
{
    mLastBackfillRange.reset();

    if (!batch.newKeys.empty())
    {
        mConfiguration.AppendKeys(batch.newKeys);
    }

    // Pre-append snapshot so per-batch back-fill restricts to the new slice.
    // First-observation columns still back-fill all rows (older rows pre-date
    // them) — across **both** row vectors when streaming is mixed with the
    // file path (test 2.7).
    const size_t oldLineCount = mData.Lines().size();
    const size_t oldStreamLineCount = mData.StreamLines().size();

    if (!batch.lines.empty() || !batch.localLineOffsets.empty())
    {
        mData.AppendBatch(std::move(batch.lines), std::move(batch.localLineOffsets));
    }
    if (!batch.streamLines.empty())
    {
        mData.AppendBatch(std::move(batch.streamLines));
    }

    if (!batch.newKeys.empty())
    {
        RefreshColumnKeyIdsForKeys(batch.newKeys);
    }

    // Back-fill post-snapshot time columns (Stage B handles snapshot ones):
    //   1. First observation: back-fill all rows and record `mLastBackfillRange`
    //      so `dataChanged` covers existing rows.
    //   2. Already-known post-snapshot column: back-fill only the appended
    //      slice; the upcoming `beginInsertRows` already invalidates them.
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

        bool stageBHandled = false;
        bool firstObservation = false;
        bool needsSliceBackfill = false;
        std::vector<KeyId> columnKeyIds;
        columnKeyIds.reserve(column.keys.size());
        for (const std::string &key : column.keys)
        {
            const KeyId id = mData.Keys().Find(key);
            columnKeyIds.push_back(id);
            if (id == kInvalidKeyId)
            {
                continue;
            }
            if (mStageBSnapshotTimeKeys.contains(id))
            {
                stageBHandled = true;
                continue;
            }
            if (mPostSnapshotTimeKeys.contains(id))
            {
                needsSliceBackfill = true;
            }
            else
            {
                firstObservation = true;
            }
        }

        if (stageBHandled)
        {
            continue;
        }

        // Use the `Discard` overload: streaming has no consumer for per-line errors.
        if (firstObservation)
        {
            BackfillTimestampColumn(column, std::span<LogLine>(mData.Lines()), BackfillErrors::Discard);
            BackfillTimestampColumn(column, std::span<StreamLogLine>(mData.StreamLines()), BackfillErrors::Discard);
            for (const KeyId id : columnKeyIds)
            {
                if (id != kInvalidKeyId)
                {
                    mPostSnapshotTimeKeys.insert(id);
                }
            }
            if (!firstBackfilled.has_value())
            {
                firstBackfilled = columnIndex;
            }
            lastBackfilled = columnIndex;
        }
        else if (needsSliceBackfill)
        {
            if (oldLineCount < mData.Lines().size())
            {
                std::span<LogLine> slice(mData.Lines().data() + oldLineCount, mData.Lines().size() - oldLineCount);
                BackfillTimestampColumn(column, slice, BackfillErrors::Discard);
            }
            if (oldStreamLineCount < mData.StreamLines().size())
            {
                std::span<StreamLogLine> slice(
                    mData.StreamLines().data() + oldStreamLineCount, mData.StreamLines().size() - oldStreamLineCount
                );
                BackfillTimestampColumn(column, slice, BackfillErrors::Discard);
            }
        }
    }

    if (firstBackfilled.has_value())
    {
        mLastBackfillRange = std::make_pair(*firstBackfilled, *lastBackfilled);
    }
}

LogTable::AppendBatchPreview LogTable::PreviewAppend(const StreamedBatch &batch) const
{
    AppendBatchPreview preview;
    // Predicted row count includes both row kinds. Today the streaming GUI
    // path always picks one or the other for a given session, but tests and
    // future multi-source designs may mix them; the preview must reflect the
    // post-`AppendBatch` row count regardless (PRD G7 / 4.9.7).
    preview.newRowCount =
        mData.Lines().size() + mData.StreamLines().size() + batch.lines.size() + batch.streamLines.size();
    preview.newColumnCount =
        mConfiguration.Configuration().columns.size() + mConfiguration.CountAppendableKeys(batch.newKeys);
    return preview;
}

const std::optional<std::pair<size_t, size_t>> &LogTable::LastBackfillRange() const noexcept
{
    return mLastBackfillRange;
}

void LogTable::MoveColumn(size_t srcIndex, size_t destIndex)
{
    if (srcIndex == destIndex || srcIndex >= mColumnKeyIds.size() || destIndex >= mColumnKeyIds.size())
    {
        return;
    }
    mConfiguration.MoveColumn(srcIndex, destIndex);
    auto begin = mColumnKeyIds.begin();
    if (srcIndex > destIndex)
    {
        std::rotate(begin + destIndex, begin + srcIndex, begin + srcIndex + 1);
    }
    else
    {
        std::rotate(begin + srcIndex, begin + srcIndex + 1, begin + destIndex + 1);
    }
}

void LogTable::ReserveLineOffsets(size_t count)
{
    if (count == 0 || mData.Files().empty())
    {
        return;
    }
    mData.Files().front()->ReserveLineOffsets(count);
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
    const size_t fileRowCount = mData.Lines().size();
    const auto resolveValue = [&](auto &&getter) -> LogValue {
        for (const KeyId id : mColumnKeyIds[column])
        {
            if (id == kInvalidKeyId)
            {
                continue;
            }
            LogValue value = getter(id);
            if (!std::holds_alternative<std::monostate>(value))
            {
                return value;
            }
        }
        return std::monostate{};
    };
    if (row < fileRowCount)
    {
        const auto &line = mData.Lines()[row];
        return resolveValue([&](KeyId id) { return line.GetValue(id); });
    }
    const size_t streamRow = row - fileRowCount;
    const auto &line = mData.StreamLines()[streamRow];
    return resolveValue([&](KeyId id) { return line.GetValue(id); });
}

std::string LogTable::GetFormattedValue(size_t row, size_t column) const
{
    if (column >= mColumnKeyIds.size())
    {
        return "";
    }
    const std::string &printFormat = mConfiguration.Configuration().columns.at(column).printFormat;
    const size_t fileRowCount = mData.Lines().size();
    const auto resolveFormatted = [&](auto &&getter) -> std::string {
        for (const KeyId id : mColumnKeyIds[column])
        {
            if (id == kInvalidKeyId)
            {
                continue;
            }
            LogValue value = getter(id);
            if (!std::holds_alternative<std::monostate>(value))
            {
                return FormatLogValue(printFormat, value);
            }
        }
        return std::string{};
    };
    if (row < fileRowCount)
    {
        const auto &line = mData.Lines()[row];
        return resolveFormatted([&](KeyId id) { return line.GetValue(id); });
    }
    const size_t streamRow = row - fileRowCount;
    const auto &line = mData.StreamLines()[streamRow];
    return resolveFormatted([&](KeyId id) { return line.GetValue(id); });
}

size_t LogTable::RowCount() const
{
    return mData.Lines().size() + mData.StreamLines().size();
}

const LogData &LogTable::Data() const noexcept
{
    return mData;
}

void LogTable::EvictPrefixRows(size_t count)
{
    if (count == 0)
    {
        return;
    }
    auto &fileLines = mData.Lines();
    auto &streamLines = mData.StreamLines();
    const size_t fileRowCount = fileLines.size();
    if (count >= fileRowCount + streamLines.size())
    {
        fileLines.clear();
        streamLines.clear();
        return;
    }
    if (fileRowCount > 0)
    {
        const size_t fileDrop = std::min(count, fileRowCount);
        fileLines.erase(fileLines.begin(), fileLines.begin() + static_cast<std::ptrdiff_t>(fileDrop));
        count -= fileDrop;
    }
    if (count > 0)
    {
        streamLines.erase(streamLines.begin(), streamLines.begin() + static_cast<std::ptrdiff_t>(count));
    }
}

KeyIndex &LogTable::Keys()
{
    return mData.Keys();
}

const KeyIndex &LogTable::Keys() const
{
    return mData.Keys();
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
    mStageBSnapshotTimeKeys.clear();
    mPostSnapshotTimeKeys.clear();
    const auto &columns = mConfiguration.Configuration().columns;
    for (const auto &column : columns)
    {
        if (column.type != LogConfiguration::Type::time)
        {
            continue;
        }
        for (const std::string &key : column.keys)
        {
            // `GetOrInsert` so the snapshot holds valid ids on the fresh
            // post-`BeginStreaming` `KeyIndex` (mirrors `BuildTimeColumnSpecs`).
            const KeyId id = mData.Keys().GetOrInsert(key);
            mStageBSnapshotTimeKeys.insert(id);
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
