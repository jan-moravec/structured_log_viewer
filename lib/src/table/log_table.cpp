#include "loglib/table/log_table.hpp"

#include "loglib/core/compact_log_value.hpp"
#include "loglib/core/file_line_source.hpp"
#include "loglib/table/internal/column_key_id_cache.hpp"
#include "loglib/table/internal/timestamp_backfill.hpp"
#include "loglib/time/time_zone_context.hpp"

#include <fmt/format.h>

#include <algorithm>
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

LogTable::LogTable(LogConfiguration configuration)
{
    mConfiguration.SetConfiguration(std::move(configuration));
}

// NOLINTNEXTLINE(bugprone-exception-escape)
LogTable::LogTable(LogTable &&other) noexcept
    : mData(std::move(other.mData)),
      mConfiguration(std::move(other.mConfiguration)),
      mDisplayTimeZone(std::move(other.mDisplayTimeZone)),
      mDefaultSourceTimeZone(std::move(other.mDefaultSourceTimeZone)),
      mReferenceTime(other.mReferenceTime),
      mReferenceTimeFixed(other.mReferenceTimeFixed),
      mColumnKeyIds(std::move(other.mColumnKeyIds)),
      mTableConvertedTimeKeys(std::move(other.mTableConvertedTimeKeys)),
      mEnum(std::move(other.mEnum))
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
    mDisplayTimeZone = std::move(other.mDisplayTimeZone);
    mDefaultSourceTimeZone = std::move(other.mDefaultSourceTimeZone);
    mReferenceTime = other.mReferenceTime;
    mReferenceTimeFixed = other.mReferenceTimeFixed;
    mColumnKeyIds = std::move(other.mColumnKeyIds);
    mTableConvertedTimeKeys = std::move(other.mTableConvertedTimeKeys);
    mEnum = std::move(other.mEnum);
    RewireSourceRegistries();
    return *this;
}

void LogTable::Reset()
{
    mData = LogData{};
    mTableConvertedTimeKeys.clear();
    mEnum.Clear();
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
    mEnum.BeginStreaming();
    if (!mReferenceTimeFixed)
    {
        mReferenceTime = std::chrono::system_clock::now();
    }

    if (source)
    {
        std::vector<LogLine> noLines;
        mData = LogData(std::move(source), std::move(noLines), KeyIndex{});
    }
    else
    {
        mData = LogData{};
    }

    RewireSourceRegistries();
    mTableConvertedTimeKeys.clear();
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

    source->SetEnumDictionaries(&mEnum.Dictionaries());
    mData.Sources().push_back(std::move(source));
}

LogTable::AppendBatchResult LogTable::AppendBatch(RowBatch batch)
{
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

        bool parserConverted = false;
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
            if (std::ranges::contains(batch.convertedTimeKeys, id))
            {
                parserConverted = true;
                continue;
            }
            if (mTableConvertedTimeKeys.contains(id))
            {
                needsSliceBackfill = true;
            }
            else
            {
                firstObservation = true;
            }
        }

        if (parserConverted)
        {
            continue;
        }

        if (firstObservation)
        {
            BackfillTimestampColumn(
                mData.Keys(),
                column,
                std::span<LogLine>(mData.Lines()),
                mDefaultSourceTimeZone,
                mReferenceTime,
                BackfillErrors::Discard
            );
            for (const KeyId id : columnKeyIds)
            {
                if (id != INVALID_KEY_ID)
                {
                    mTableConvertedTimeKeys.insert(id);
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
                BackfillTimestampColumn(
                    mData.Keys(), column, slice, mDefaultSourceTimeZone, mReferenceTime, BackfillErrors::Discard
                );
            }
        }
    }

    mEnum.RunPassForAppendBatch(mData, mConfiguration, mColumnKeyIds, oldLineCount, firstBackfilled, lastBackfilled);

    AppendBatchResult result;
    if (firstBackfilled.has_value())
    {
        result.backfilledColumns = std::make_pair(*firstBackfilled, *lastBackfilled);
    }
    result.demotedKeys = mEnum.LastBatchDemotedKeys();
    result.levelBubbleKeys = mEnum.TakePendingLevelBubbleKeys();
    return result;
}

LogTable::AppendBatchPreview LogTable::PreviewAppend(const RowBatch &batch) const
{
    AppendBatchPreview preview;
    preview.newRowCount = mData.Lines().size() + batch.lines.size();
    preview.newColumnCount =
        mConfiguration.Configuration().columns.size() + mConfiguration.CountAppendableKeys(batch.newKeys);
    return preview;
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
        fileSource->ReserveLineOffsets(count);
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

BytesProducer *LogTable::LiveProducer() noexcept
{
    if (StreamLineSource *streamSource = mData.BackStreamSource(); streamSource != nullptr)
    {
        return streamSource->Producer();
    }
    return nullptr;
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
    auto &lines = mData.Lines();
    const size_t evicted = std::min(count, lines.size());

    // Line ids are per source, so each source is trimmed by its own rows. A
    // surviving row may belong to a different source than the evicted ones.
    std::vector<std::pair<LineSource *, size_t>> lastEvicted;
    for (size_t row = 0; row < evicted; ++row)
    {
        LineSource *source = lines[row].Source();
        if (source == nullptr || !source->SupportsEviction())
        {
            continue;
        }
        const auto it = std::ranges::find(lastEvicted, source, &std::pair<LineSource *, size_t>::first);
        if (it == lastEvicted.end())
        {
            lastEvicted.emplace_back(source, lines[row].LineId());
        }
        else
        {
            it->second = std::max(it->second, lines[row].LineId());
        }
    }

    lines.erase(lines.begin(), lines.begin() + static_cast<std::ptrdiff_t>(evicted));
    for (const auto &[source, lineId] : lastEvicted)
    {
        source->EvictBefore(lineId + 1);
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

void LogTable::SetDisplayTimeZone(TimeZoneContext timeZone)
{
    mDisplayTimeZone = std::move(timeZone);
}

const TimeZoneContext &LogTable::DisplayTimeZone() const noexcept
{
    return mDisplayTimeZone;
}

void LogTable::SetDefaultSourceTimeZone(TimeZoneContext timeZone)
{
    mDefaultSourceTimeZone = std::move(timeZone);
}

const TimeZoneContext &LogTable::DefaultSourceTimeZone() const noexcept
{
    return mDefaultSourceTimeZone;
}

void LogTable::SetReferenceTime(std::chrono::system_clock::time_point referenceTime) noexcept
{
    mReferenceTime = referenceTime;
    mReferenceTimeFixed = true;
}

std::chrono::system_clock::time_point LogTable::ReferenceTime() const noexcept
{
    return mReferenceTime;
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
    internal::RefreshColumnKeyIdsForKeys(mColumnKeyIds, mConfiguration.Configuration(), mData.Keys(), newKeys);
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

LogTable::FinalizeResult LogTable::FinalizeAutoDetection()
{
    FinalizeResult result;
    result.promotedToEnumeration = mEnum.Finalize(mData, mConfiguration);
    result.levelBubbleKeys = mEnum.TakePendingLevelBubbleKeys();
    return result;
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
            mData.Keys(),
            mConfiguration.Configuration().columns[columnIndex],
            std::span<LogLine>(mData.Lines()),
            mDefaultSourceTimeZone,
            mReferenceTime,
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
        const bool previousWasEnumLike = previousType == ColumnType::Enumeration || previousType == ColumnType::Level;
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

std::vector<size_t> LogTable::TimeColumnsToFront(size_t firstColumn) const
{
    std::vector<size_t> moves;
    const auto &columns = mConfiguration.Configuration().columns;
    for (size_t columnIndex = std::max<size_t>(firstColumn, 1); columnIndex < columns.size(); ++columnIndex)
    {
        if (columns[columnIndex].type == ColumnType::Time)
        {
            moves.push_back(columnIndex);
        }
    }
    return moves;
}

void LogTable::AppendBatchArranged(RowBatch batch)
{
    const size_t oldColumnCount = ColumnCount();
    const AppendBatchResult result = AppendBatch(std::move(batch));
    for (const size_t srcIndex : TimeColumnsToFront(oldColumnCount))
    {
        MoveColumn(srcIndex, 0);
    }
    BubbleLevelColumns(result.levelBubbleKeys);
}

void LogTable::BubbleLevelColumns(std::span<const KeyId> keys)
{
    for (const KeyId kid : keys)
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

std::string LogTable::FormatLogValue(const std::string &format, const LogValue &value) const
{
    return std::visit(
        [&](const auto &arg) -> std::string {
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
                try
                {
                    return fmt::vformat(format, fmt::make_format_args(arg));
                }
                catch (const fmt::format_error &)
                {
                    return fmt::format("{}", arg);
                }
            }
            else if constexpr (std::is_same_v<T, TimeStamp>)
            {
                try
                {
                    return mDisplayTimeZone.Format(arg, format);
                }
                catch (const std::exception &)
                {
                    return mDisplayTimeZone.FormatTimeStamp(arg);
                }
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
