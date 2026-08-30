#include "loglib/log_table.hpp"

#include "loglib/compact_log_value.hpp"
#include "loglib/file_line_source.hpp"
#include "loglib/internal/column_key_id_cache.hpp"
#include "loglib/log_processing.hpp"
#include "loglib/time_zone_context.hpp"

#include <fmt/format.h>

#include <cassert>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace loglib
{

LogTable::LogTable(LogData data, LogConfigurationManager configuration)
    : mData(std::move(data)), mConfiguration(std::move(configuration))
{
    mEnum.ClearLastBatchDemotedKeys();
    RewireSourceRegistries();
    mEnum.RefreshSnapshot(mData, mConfiguration);
    RefreshColumnKeyIds();
    std::optional<size_t> firstBackfilled;
    std::optional<size_t> lastBackfilled;
    mEnum.RunPassForAppendBatch(
        mData, mConfiguration, mColumnKeyIds, 0U, firstBackfilled, lastBackfilled
    );
    mEnum.Finalize(mData, mConfiguration);
    ApplyPendingLevelBubbles();
}

// NOLINTNEXTLINE(bugprone-exception-escape)
LogTable::LogTable(LogTable &&other) noexcept
    : mData(std::move(other.mData)),
      mConfiguration(std::move(other.mConfiguration)),
      mColumnKeyIds(std::move(other.mColumnKeyIds)),
      mStageBSnapshotTimeKeys(std::move(other.mStageBSnapshotTimeKeys)),
      mPostSnapshotTimeKeys(std::move(other.mPostSnapshotTimeKeys)),
      mEnum(std::move(other.mEnum)),
      mLastBackfillRange(std::move(other.mLastBackfillRange))
{
    RewireSourceRegistries();
}

LogTable &LogTable::operator=(LogTable &&other) noexcept
{
    if (this == &other)
    {
        return *this;
    }
    mData = std::move(other.mData);
    mConfiguration = std::move(other.mConfiguration);
    mColumnKeyIds = std::move(other.mColumnKeyIds);
    mStageBSnapshotTimeKeys = std::move(other.mStageBSnapshotTimeKeys);
    mPostSnapshotTimeKeys = std::move(other.mPostSnapshotTimeKeys);
    mEnum = std::move(other.mEnum);
    mLastBackfillRange = std::move(other.mLastBackfillRange);
    RewireSourceRegistries();
    return *this;
}

void LogTable::Update(LogData &&data)
{
    mEnum.ClearLastBatchDemotedKeys();
    const size_t oldLineCount = mData.Lines().size();
    mConfiguration.Update(data);
    if (!data.TimestampsAlreadyParsed())
    {
        ParseTimestamps(data, mConfiguration.Configuration());
    }
    mData.Merge(std::move(data));
    RewireSourceRegistries();
    mEnum.RefreshSnapshot(mData, mConfiguration);
    RefreshColumnKeyIds();
    std::optional<size_t> firstBackfilled;
    std::optional<size_t> lastBackfilled;
    mEnum.RunPassForAppendBatch(
        mData, mConfiguration, mColumnKeyIds, oldLineCount, firstBackfilled, lastBackfilled
    );
    mEnum.Finalize(mData, mConfiguration);
    ApplyPendingLevelBubbles();
}

void LogTable::Reset()
{
    mData = LogData{};
    mStageBSnapshotTimeKeys.clear();
    mPostSnapshotTimeKeys.clear();
    mEnum.Clear();
    mLastBackfillRange.reset();
    RefreshColumnKeyIds();
    mEnum.RefreshSnapshot(mData, mConfiguration);
}

void LogTable::OnConfigurationReloaded()
{
    RefreshColumnKeyIds();
    mEnum.RefreshSnapshot(mData, mConfiguration);
}

void LogTable::BeginStreaming(std::unique_ptr<LineSource> source)
{
    mLastBackfillRange.reset();
    mEnum.BeginStreaming();

    if (source)
    {
        std::vector<LogLine> noLines;
        LogData fresh(std::move(source), std::move(noLines), KeyIndex{});
        fresh.MarkTimestampsParsed();
        mData = std::move(fresh);
    }
    else
    {
        mData = LogData{};
        mData.MarkTimestampsParsed();
    }

    RewireSourceRegistries();
    RefreshSnapshotTimeKeys();
    mEnum.RefreshSnapshot(mData, mConfiguration);
    RefreshColumnKeyIds();
}

void LogTable::AppendStreaming(std::unique_ptr<LineSource> source)
{
    assert(source != nullptr);
    if (source == nullptr)
    {
        return;
    }

    mLastBackfillRange.reset();
    source->SetEnumDictionaries(&mEnum.Dictionaries());
    mData.Sources().push_back(std::move(source));
}

void LogTable::AppendBatch(StreamedBatch batch)
{
    mLastBackfillRange.reset();
    mEnum.ClearLastBatchDemotedKeys();
    (void)mEnum.TakePendingLevelBubbleKeys();

    if (!batch.newKeys.empty())
    {
        mConfiguration.AppendKeys(batch.newKeys);
    }

    const size_t oldLineCount = mData.Lines().size();

    if (!batch.lines.empty() || !batch.localLineOffsets.empty() || !batch.multiLineSpans.empty())
    {
        mData.AppendBatch(std::move(batch.lines), batch.localLineOffsets, batch.multiLineSpans);
    }

    if (!batch.newKeys.empty())
    {
        RefreshColumnKeyIdsForKeys(batch.newKeys);
    }

    const auto &columns = mConfiguration.Configuration().columns;
    std::optional<size_t> firstBackfilled;
    std::optional<size_t> lastBackfilled;
    for (size_t columnIndex = 0; columnIndex < columns.size(); ++columnIndex)
    {
        const auto &column = columns[columnIndex];
        if (column.type != ColumnType::Time)
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
            if (id == INVALID_KEY_ID)
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

        if (firstObservation)
        {
            BackfillTimestampColumn(column, std::span<LogLine>(mData.Lines()), BackfillErrors::Discard);
            for (const KeyId id : columnKeyIds)
            {
                if (id != INVALID_KEY_ID)
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
                const std::span<LogLine> slice(
                    mData.Lines().data() + oldLineCount, mData.Lines().size() - oldLineCount
                );
                BackfillTimestampColumn(column, slice, BackfillErrors::Discard);
            }
        }
    }

    mEnum.RunPassForAppendBatch(
        mData, mConfiguration, mColumnKeyIds, oldLineCount, firstBackfilled, lastBackfilled
    );

    if (firstBackfilled.has_value())
    {
        mLastBackfillRange = std::make_pair(*firstBackfilled, *lastBackfilled);
    }
}

LogTable::AppendBatchPreview LogTable::PreviewAppend(const StreamedBatch &batch) const
{
    AppendBatchPreview preview;
    preview.newRowCount = mData.Lines().size() + batch.lines.size();
    preview.newColumnCount =
        mConfiguration.Configuration().columns.size() + mConfiguration.CountAppendableKeys(batch.newKeys);
    return preview;
}

const std::optional<std::pair<size_t, size_t>> &LogTable::LastBackfillRange() const noexcept
{
    return mLastBackfillRange;
}

const std::vector<KeyId> &LogTable::LastBatchDemotedKeys() const noexcept
{
    return mEnum.LastBatchDemotedKeys();
}

void LogTable::MoveColumn(size_t srcIndex, size_t destIndex)
{
    if (srcIndex == destIndex || srcIndex >= mColumnKeyIds.size() || destIndex >= mColumnKeyIds.size())
    {
        return;
    }
    mConfiguration.MoveColumn(srcIndex, destIndex);
    internal::MoveColumnKeyIds(mColumnKeyIds, srcIndex, destIndex);
}

void LogTable::ReserveLineOffsets(size_t count)
{
    if (count == 0)
    {
        return;
    }
    if (FileLineSource *fileSource = mData.FrontFileSource(); fileSource != nullptr)
    {
        fileSource->File().ReserveLineOffsets(count);
    }
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
    if (column >= mColumnKeyIds.size() || row >= mData.Lines().size())
    {
        return std::monostate{};
    }
    const auto &line = mData.Lines()[row];
    for (const KeyId id : mColumnKeyIds[column])
    {
        if (id == INVALID_KEY_ID)
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
    if (column >= mColumnKeyIds.size() || row >= mData.Lines().size())
    {
        return "";
    }
    const std::string &printFormat = mConfiguration.Configuration().columns.at(column).printFormat;
    const auto &line = mData.Lines()[row];
    for (const KeyId id : mColumnKeyIds[column])
    {
        if (id == INVALID_KEY_ID)
        {
            continue;
        }
        const LogValue value = line.GetValue(id);
        if (!std::holds_alternative<std::monostate>(value))
        {
            return FormatLogValue(printFormat, value);
        }
    }
    return std::string{};
}

std::string_view LogTable::GetValueOrFormatted(size_t row, size_t column, std::string &buffer) const
{
    if (column >= mColumnKeyIds.size() || row >= mData.Lines().size())
    {
        return {};
    }
    const auto &columns = mConfiguration.Configuration().columns;
    const auto &line = mData.Lines()[row];
    for (const KeyId id : mColumnKeyIds[column])
    {
        if (id == INVALID_KEY_ID)
        {
            continue;
        }
        LogValue value = line.GetValue(id);
        if (std::holds_alternative<std::monostate>(value))
        {
            continue;
        }
        if (const auto *sv = std::get_if<std::string_view>(&value); sv != nullptr)
        {
            return *sv;
        }
        if (const auto *s = std::get_if<std::string>(&value); s != nullptr)
        {
            buffer.assign(*s);
            return buffer;
        }
        buffer = FormatLogValue(columns[column].printFormat, value);
        return buffer;
    }
    return {};
}

size_t LogTable::RowCount() const
{
    return mData.Lines().size();
}

const LogData &LogTable::Data() const noexcept
{
    return mData;
}

LogData &LogTable::Data() noexcept
{
    return mData;
}

void LogTable::EvictPrefixRows(size_t count)
{
    if (count == 0)
    {
        return;
    }
    auto &lines = mData.Lines();

    auto evictSource = [&](size_t firstSurvivingLineId) {
        for (auto &source : mData.Sources())
        {
            if (source != nullptr && source->SupportsEviction())
            {
                source->EvictBefore(firstSurvivingLineId);
            }
        }
    };

    if (count >= lines.size())
    {
        size_t firstSurvivingLineId = 0;
        if (!lines.empty())
        {
            firstSurvivingLineId = lines.back().LineId() + 1;
        }
        lines.clear();
        evictSource(firstSurvivingLineId);
        return;
    }

    const size_t firstSurvivingLineId = lines[count].LineId();
    lines.erase(lines.begin(), lines.begin() + static_cast<std::ptrdiff_t>(count));
    evictSource(firstSurvivingLineId);
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
    internal::RefreshColumnKeyIds(mColumnKeyIds, mConfiguration.Configuration(), mData.Keys());
}

void LogTable::RefreshColumnKeyIdsForKeys(const std::vector<std::string> &newKeys)
{
    internal::RefreshColumnKeyIdsForKeys(
        mColumnKeyIds, mConfiguration.Configuration(), mData.Keys(), newKeys
    );
}

void LogTable::RefreshSnapshotTimeKeys()
{
    mStageBSnapshotTimeKeys.clear();
    mPostSnapshotTimeKeys.clear();
    const auto &columns = mConfiguration.Configuration().columns;
    for (const auto &column : columns)
    {
        if (column.type != ColumnType::Time)
        {
            continue;
        }
        for (const std::string &key : column.keys)
        {
            const KeyId id = mData.Keys().GetOrInsert(key);
            mStageBSnapshotTimeKeys.insert(id);
        }
    }
}

void LogTable::RewireSourceRegistries()
{
    for (auto &source : mData.Sources())
    {
        if (source != nullptr)
        {
            source->SetEnumDictionaries(&mEnum.Dictionaries());
        }
    }
}

const EnumDictionaryRegistry &LogTable::EnumDictionaries() const noexcept
{
    return mEnum.Dictionaries();
}

void LogTable::SetEnumValueCap(uint16_t cap) noexcept
{
    mEnum.SetValueCap(cap);
}

uint16_t LogTable::EnumValueCap() const noexcept
{
    return mEnum.ValueCap();
}

void LogTable::SetEnumValueMaxLen(uint32_t maxLen) noexcept
{
    mEnum.SetValueMaxLen(maxLen);
}

uint32_t LogTable::EnumValueMaxLen() const noexcept
{
    return mEnum.ValueMaxLen();
}

std::optional<EnumValueId> LogTable::GetEnumValueId(size_t row, size_t column) const noexcept
{
    if (column >= mColumnKeyIds.size() || row >= mData.Lines().size())
    {
        return std::nullopt;
    }
    const auto &line = mData.Lines()[row];
    for (const KeyId id : mColumnKeyIds[column])
    {
        if (id == INVALID_KEY_ID)
        {
            continue;
        }
        if (auto vid = line.GetEnumValueId(id); vid.has_value())
        {
            return vid;
        }
    }
    return std::nullopt;
}

LogTable::EnumColumnLookup LogTable::ResolveEnumColumn(size_t columnIndex) const noexcept
{
    const auto &columns = mConfiguration.Configuration().columns;
    if (columnIndex >= columns.size())
    {
        return {};
    }
    const auto &column = columns[columnIndex];
    if (column.keys.empty())
    {
        return {};
    }
    const KeyId canonicalKey = Keys().Find(column.keys.front());
    if (canonicalKey == INVALID_KEY_ID)
    {
        return {};
    }
    return {.canonicalKey = canonicalKey, .dictionary = mEnum.Dictionaries().Find(canonicalKey)};
}

LogTable::ColumnTypeHealth LogTable::ComputeColumnTypeHealth(size_t columnIndex) const
{
    const auto &columns = mConfiguration.Configuration().columns;
    if (columnIndex >= columns.size())
    {
        return {};
    }
    return loglib::ComputeColumnTypeHealth(mData.Lines(), mData.Keys(), columns[columnIndex]);
}

ColumnType LogTable::RescanColumnForAutoDetection(size_t columnIndex)
{
    return mEnum.RescanColumn(mData, mConfiguration, mColumnKeyIds, columnIndex);
}

bool LogTable::FinalizeAutoDetection()
{
    return mEnum.Finalize(mData, mConfiguration);
}

void LogTable::OnUserChangedColumnType(size_t columnIndex, ColumnType previousType)
{
    const auto &columns = mConfiguration.Configuration().columns;
    if (columnIndex >= columns.size())
    {
        return;
    }
    const auto snapshot = [this, columnIndex]() -> Column {
        return mConfiguration.Configuration().columns[columnIndex];
    };

    {
        const auto col = snapshot();
        if (!col.keys.empty())
        {
            const KeyId canonical = mData.Keys().Find(col.keys.front());
            if (canonical != INVALID_KEY_ID)
            {
                mEnum.EraseTracker(canonical);
            }
        }
    }

    RefreshColumnKeyIds();

    const auto column = snapshot();
    switch (column.type)
    {
    case ColumnType::Time:
    {
        if (column.printFormat.empty())
        {
            mConfiguration.SetColumnPrintFormat(columnIndex, "%F %H:%M:%S");
        }
        if (column.parseFormats.empty())
        {
            mConfiguration.SetColumnParseFormats(columnIndex, DefaultTimeParseFormats());
        }
        BackfillTimestampColumn(
            mConfiguration.Configuration().columns[columnIndex],
            std::span<LogLine>(mData.Lines()),
            BackfillErrors::Discard
        );
        break;
    }
    case ColumnType::Enumeration:
    case ColumnType::Level:
    {
        mEnum.RefreshSnapshot(mData, mConfiguration);
        RefreshColumnKeyIds();
        if (column.keys.empty())
        {
            break;
        }
        const KeyId canonical = mData.Keys().Find(column.keys.front());
        if (canonical == INVALID_KEY_ID)
        {
            break;
        }
        EnumColumnHealth &health = mEnum.HealthFor(canonical);
        const bool previousWasEnumLike =
            previousType == ColumnType::Enumeration || previousType == ColumnType::Level;
        if (!previousWasEnumLike)
        {
            health = EnumColumnHealth{};
        }
        if (!mEnum.EncodeColumnRangeAsEnum(
                mData, mConfiguration.Configuration().columns[columnIndex], 0U, mData.Lines().size(), health
            ))
        {
            mEnum.DemoteColumnFromEnum(mData, mConfiguration, columnIndex, /*recordForBatch=*/false);
            return;
        }
        if (column.type == ColumnType::Level)
        {
            mEnum.RefreshLevelRankCache(mData, mConfiguration.Configuration(), columnIndex);
        }
        break;
    }
    case ColumnType::Any:
    case ColumnType::String:
    case ColumnType::Boolean:
    case ColumnType::Integer:
    case ColumnType::Floating:
    case ColumnType::Number:
    {
        if (previousType == ColumnType::Time)
        {
            mConfiguration.SetColumnPrintFormat(columnIndex, "{}");
            mConfiguration.SetColumnParseFormats(columnIndex, {});
        }

        if (column.keys.empty())
        {
            break;
        }
        const KeyId canonical = mData.Keys().Find(column.keys.front());
        if (canonical == INVALID_KEY_ID)
        {
            break;
        }
        if (mEnum.Dictionaries().Find(canonical) != nullptr)
        {
            const auto targetType = column.type;
            mConfiguration.SetColumnType(columnIndex, ColumnType::Enumeration);
            mEnum.DemoteColumnFromEnum(mData, mConfiguration, columnIndex, /*recordForBatch=*/false);
            mConfiguration.SetColumnType(columnIndex, targetType);
        }
        mEnum.EraseHealthAndLevelCache(canonical);
        break;
    }
    }
}

std::vector<KeyId> LogTable::TakePendingLevelBubbleKeys() noexcept
{
    return mEnum.TakePendingLevelBubbleKeys();
}

void LogTable::ApplyPendingLevelBubbles()
{
    const std::vector<KeyId> pending = TakePendingLevelBubbleKeys();
    for (const KeyId kid : pending)
    {
        const int srcIndex = FindColumnIndexByKey(kid);
        if (srcIndex < 0)
        {
            continue;
        }
        if (ShouldBubbleLevelColumn(mConfiguration.Configuration(), static_cast<size_t>(srcIndex)))
        {
            MoveColumn(static_cast<size_t>(srcIndex), CANONICAL_LEVEL_COLUMN_INDEX);
        }
    }
}

int LogTable::FindColumnIndexByKey(KeyId kid) const noexcept
{
    if (kid == INVALID_KEY_ID)
    {
        return -1;
    }
    const auto &columns = mConfiguration.Configuration().columns;
    const auto &keys = mData.Keys();
    for (size_t i = 0; i < columns.size(); ++i)
    {
        for (const std::string &key : columns[i].keys)
        {
            if (keys.Find(key) == kid)
            {
                return static_cast<int>(i);
            }
        }
    }
    return -1;
}

std::optional<LogLevel> LogTable::GetLevelForRow(size_t row, size_t columnIndex) const noexcept
{
    const auto level = GetDisplayLevelForRow(row, columnIndex);
    if (!level.has_value() || *level == LogLevel::Unknown)
    {
        return std::nullopt;
    }
    return level;
}

std::optional<LogLevel> LogTable::GetDisplayLevelForRow(size_t row, size_t columnIndex) const noexcept
{
    const auto *ranks = mEnum.LevelRankCache(mData, mConfiguration.Configuration(), columnIndex);
    if (ranks == nullptr)
    {
        return std::nullopt;
    }
    const std::optional<EnumValueId> id = GetEnumValueId(row, columnIndex);
    if (!id.has_value())
    {
        return std::nullopt;
    }
    if (static_cast<size_t>(*id) >= ranks->size())
    {
        return LogLevel::Unknown;
    }
    return (*ranks)[static_cast<size_t>(*id)];
}

const std::vector<LogLevel> *LogTable::LevelRankCache(size_t columnIndex) const noexcept
{
    return mEnum.LevelRankCache(mData, mConfiguration.Configuration(), columnIndex);
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
            else if constexpr (
                std::is_same_v<T, int64_t> || std::is_same_v<T, uint64_t> || std::is_same_v<T, double> ||
                std::is_same_v<T, bool>
            )
            {
                return fmt::vformat(format, fmt::make_format_args(arg));
            }
            else if constexpr (std::is_same_v<T, TimeStamp>)
            {
                return ProcessDefaultTimeZone().Format(arg, format);
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

} // namespace loglib
