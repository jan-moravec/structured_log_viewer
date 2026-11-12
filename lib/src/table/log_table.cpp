#include "loglib/table/log_table.hpp"

#include "loglib/core/compact_log_value.hpp"
#include "loglib/core/file_line_source.hpp"
#include "loglib/schema/internal/level_column_slot.hpp"
#include "loglib/table/internal/column_key_id_cache.hpp"
#include "loglib/table/internal/enum_inference.hpp"
#include "loglib/table/internal/timestamp_backfill.hpp"
#include "loglib/time/time_zone_context.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <exception>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace loglib
{

namespace
{

/** Room for any 64-bit integer in decimal, with its sign. */
constexpr std::size_t INTEGER_TEXT_CAPACITY = 24;

/** Room for a double in fixed notation with up to 99 digits after the point. Longer text uses the general formatter. */
constexpr std::size_t FIXED_DOUBLE_TEXT_CAPACITY = 400;

/** The number of digits after the point when a print format is a fixed-precision one such as {:.2f}. */
std::optional<int> FixedPrecisionOf(std::string_view format) noexcept
{
    // Only {:.N} and {:.NN} followed by f are read; anything else goes to the general formatter.
    constexpr std::string_view PREFIX = "{:.";
    constexpr std::string_view SUFFIX = "f}";
    constexpr std::size_t MAX_PRECISION_DIGITS = 2;
    constexpr int DECIMAL_BASE = 10;
    if (format.size() <= PREFIX.size() + SUFFIX.size() || !format.starts_with(PREFIX) || !format.ends_with(SUFFIX))
    {
        return std::nullopt;
    }
    const std::string_view digits = format.substr(PREFIX.size(), format.size() - PREFIX.size() - SUFFIX.size());
    if (digits.size() > MAX_PRECISION_DIGITS)
    {
        return std::nullopt;
    }
    int precision = 0;
    for (const char digit : digits)
    {
        if (digit < '0' || digit > '9')
        {
            return std::nullopt;
        }
        precision = (precision * DECIMAL_BASE) + (digit - '0');
    }
    return precision;
}

/** Appends a value with a runtime print format. A format that the value does not accept gives its plain text. */
template <typename Value>
void AppendWithFmt(std::string &out, std::size_t start, const std::string &format, const Value &value)
{
    try
    {
        fmt::vformat_to(std::back_inserter(out), format, fmt::make_format_args(value));
    }
    catch (const fmt::format_error &)
    {
        out.resize(start);
        fmt::format_to(std::back_inserter(out), "{}", value);
    }
}

/** Whether a time format prints, by the same rule that FormatLogValue applies to a Time cell. */
bool TimeFormatPrints(const std::string &format)
{
    try
    {
        static_cast<void>(TimeZoneContext{}.Format(TimeStamp{}, format));
        return true;
    }
    catch (const std::exception &)
    {
        return false;
    }
}

} // namespace

LogTable::LogTable()
    : mEnum(std::make_unique<EnumInference>())
{
    mConfiguration.SetTimeFormatCheck(TimeFormatPrints);
}

LogTable::LogTable(LogConfiguration configuration)
    : mEnum(std::make_unique<EnumInference>())
{
    mConfiguration.SetTimeFormatCheck(TimeFormatPrints);
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
      mClock(std::move(other.mClock)),
      mLiveSource(other.mLiveSource),
      mColumnKeyIds(std::move(other.mColumnKeyIds)),
      mTableConvertedTimeKeys(std::move(other.mTableConvertedTimeKeys)),
      mEnum(std::make_unique<EnumInference>(std::move(*other.mEnum)))
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
    mClock = std::move(other.mClock);
    mLiveSource = other.mLiveSource;
    mColumnKeyIds = std::move(other.mColumnKeyIds);
    mTableConvertedTimeKeys = std::move(other.mTableConvertedTimeKeys);
    *mEnum = std::move(*other.mEnum);
    RewireSourceRegistries();
    return *this;
}

LogTable::~LogTable() = default;

void LogTable::Reset()
{
    mData = LogData{};
    mLiveSource = false;
    mTableConvertedTimeKeys.clear();
    mEnum->Clear();
    RefreshColumnKeyIds();
    mEnum->RefreshSnapshot(mData, mConfiguration);
}

void LogTable::OnConfigurationReloaded()
{
    RefreshColumnKeyIds();
    mEnum->RefreshSnapshot(mData, mConfiguration);
}

void LogTable::BeginStreaming(std::unique_ptr<LineSource> source)
{
    mEnum->BeginStreaming();
    mLiveSource = source && source->SupportsEviction();
    if (!mReferenceTimeFixed)
    {
        mReferenceTime = ReadClock();
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
    mEnum->RefreshSnapshot(mData, mConfiguration);
    RefreshColumnKeyIds();
}

void LogTable::AppendStreaming(std::unique_ptr<LineSource> source)
{
    assert(source != nullptr);
    if (source == nullptr)
    {
        return;
    }

    source->SetEnumDictionaries(&mEnum->Dictionaries());
    mData.Sources().push_back(std::move(source));
}

namespace
{

bool IsEnumLike(ColumnType type) noexcept
{
    return type == ColumnType::Enumeration || type == ColumnType::Level;
}

} // namespace

LogTable::AppendBatchResult LogTable::AppendBatch(RowBatch batch)
{
    if (mLiveSource && !mReferenceTimeFixed)
    {
        mReferenceTime = ReadClock();
    }
    mEnum->ClearLastBatchDemotedKeys();
    (void)mEnum->TakePendingLevelBubbleKeys();
    mEnum->ClearDemotedLevelValues();

    AppendBatchResult result;
    result.errors = std::move(batch.errors);
    batch.errors.clear();

    const size_t oldColumnCount = mConfiguration.Configuration().columns.size();
    const size_t oldRowCount = mData.Lines().size();

    // Only Enumeration and Level columns with a dictionary are recorded, so a table without them pays one pass over the
    // column types.
    std::vector<EnumColumnSnapshot> &enumSnapshot = mEnumSnapshot;
    enumSnapshot.clear();
    {
        const auto &columnsBefore = mConfiguration.Configuration().columns;
        for (size_t columnIndex = 0; columnIndex < columnsBefore.size(); ++columnIndex)
        {
            const Column &column = columnsBefore[columnIndex];
            if (!IsEnumLike(column.type))
            {
                continue;
            }
            // The cached ids give the same key as CanonicalKeyOf without hashing the key names on every batch.
            KeyId key = INVALID_KEY_ID;
            if (columnIndex < mColumnKeyIds.size())
            {
                for (const KeyId id : mColumnKeyIds[columnIndex])
                {
                    if (id != INVALID_KEY_ID)
                    {
                        key = id;
                        break;
                    }
                }
            }
            if (key == INVALID_KEY_ID)
            {
                continue;
            }
            const EnumDictionary *dictionary = mEnum->Dictionaries().Find(key);
            if (dictionary == nullptr)
            {
                continue;
            }
            enumSnapshot.push_back({
                .key = key,
                .dictionary = dictionary,
                .column = columnIndex,
                .sizeBefore = dictionary->Size(),
                .typeBefore = column.type,
            });
        }
    }

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

    mEnum->RunPassForAppendBatch(mData, mConfiguration, mColumnKeyIds, oldLineCount, firstBackfilled, lastBackfilled);

    result.rowsAdded = mData.Lines().size() - oldRowCount;
    result.columnsAdded = mConfiguration.Configuration().columns.size() - oldColumnCount;
    if (firstBackfilled.has_value())
    {
        result.backfilledColumns = std::make_pair(*firstBackfilled, *lastBackfilled);
    }
    const std::vector<KeyId> demotedKeys = mEnum->LastBatchDemotedKeys();
    const std::vector<KeyId> levelBubbleKeys = mEnum->TakePendingLevelBubbleKeys();
    for (ParseError &error : mEnum->TakePendingErrors())
    {
        result.errors.push_back(std::move(error));
    }

    // Level values exist only if a dictionary was erased, and an erase is always recorded in demotedKeys.
    std::unordered_map<KeyId, LevelValueMap> demotedValues;
    const bool anyDemotion = !demotedKeys.empty();
    if (anyDemotion)
    {
        demotedValues = mEnum->TakeDemotedLevelValues();
    }
    const auto &columnsAfter = mConfiguration.Configuration().columns;

    // Columns that were enumerations before the batch, in index order. Dictionaries stay at their address until they
    // are erased, so the recorded pointer is looked up again only when the batch demoted a column.
    std::vector<size_t> demotedColumns;
    for (const EnumColumnSnapshot &entry : enumSnapshot)
    {
        const EnumDictionary *dictionary = anyDemotion ? mEnum->Dictionaries().Find(entry.key) : entry.dictionary;
        if (dictionary == nullptr)
        {
            EnumColumnEvent event{.column = entry.column, .change = EnumColumnChange::Demoted};
            if (entry.typeBefore == ColumnType::Level)
            {
                if (const auto values = demotedValues.find(entry.key); values != demotedValues.end())
                {
                    event.levelValuesBeforeDemotion = std::move(values->second);
                }
            }
            demotedColumns.push_back(entry.column);
            result.enumEvents.push_back(std::move(event));
            continue;
        }
        const ColumnType typeAfter =
            entry.column < columnsAfter.size() ? columnsAfter[entry.column].type : entry.typeBefore;
        if (entry.typeBefore == ColumnType::Enumeration && typeAfter == ColumnType::Level)
        {
            result.enumEvents.push_back({.column = entry.column, .change = EnumColumnChange::Promoted});
        }
        else if (dictionary->Size() != entry.sizeBefore)
        {
            result.enumEvents.push_back({.column = entry.column, .change = EnumColumnChange::Grew});
        }
    }

    // A column that was promoted and demoted within the batch is invisible to the snapshot.
    for (const KeyId key : demotedKeys)
    {
        const int columnIndex = FindColumnIndexByKey(key);
        if (columnIndex < 0 || std::ranges::contains(demotedColumns, static_cast<size_t>(columnIndex)))
        {
            continue;
        }
        demotedColumns.push_back(static_cast<size_t>(columnIndex));
        result.enumEvents.push_back({.column = static_cast<size_t>(columnIndex), .change = EnumColumnChange::Demoted});
    }

    // Columns the back-fill turned into enumerations. A column that was already one is covered by the events above.
    if (result.backfilledColumns.has_value() && !mData.Lines().empty())
    {
        for (size_t columnIndex = result.backfilledColumns->first;
             columnIndex <= result.backfilledColumns->second && columnIndex < columnsAfter.size();
             ++columnIndex)
        {
            if (!IsEnumLike(columnsAfter[columnIndex].type) ||
                std::ranges::any_of(enumSnapshot, [columnIndex](const EnumColumnSnapshot &entry) {
                    return entry.column == columnIndex;
                }))
            {
                continue;
            }
            result.enumEvents.push_back(
                {.column = columnIndex, .change = EnumColumnChange::Promoted, .causedByBackfill = true}
            );
        }
    }

    for (size_t columnIndex = std::max<size_t>(oldColumnCount, 1); columnIndex < columnsAfter.size(); ++columnIndex)
    {
        const Column &column = columnsAfter[columnIndex];
        if (column.type != ColumnType::Time || column.keys.empty())
        {
            continue;
        }
        result.placements.push_back({.kind = ColumnPlacement::Kind::TimeToFront, .column = columnIndex});
    }
    for (const KeyId key : levelBubbleKeys)
    {
        if (const int columnIndex = FindColumnIndexByKey(key); columnIndex >= 0)
        {
            result.placements.push_back(
                {.kind = ColumnPlacement::Kind::LevelToCanonicalSlot, .column = static_cast<size_t>(columnIndex)}
            );
        }
    }
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

const CompactLogValue *LogTable::FindCell(size_t row, size_t column) const noexcept
{
    if (column >= mColumnKeyIds.size() || row >= mData.Lines().size())
    {
        return nullptr;
    }
    const auto &line = mData.Lines()[row];
    for (const KeyId id : mColumnKeyIds[column])
    {
        if (id == INVALID_KEY_ID)
        {
            continue;
        }
        const CompactLogValue *slot = line.FindCompact(id);
        if (slot != nullptr && slot->tag != CompactTag::Monostate)
        {
            return slot;
        }
    }
    return nullptr;
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
        const CompactLogValue *slot = line.FindCompact(id);
        if (slot == nullptr || slot->tag == CompactTag::Monostate)
        {
            continue;
        }
        if (slot->tag == CompactTag::MmapSlice || slot->tag == CompactTag::OwnedString)
        {
            const std::optional<std::string_view> bytes = line.PeekStringView(*slot);
            if (slot->tag == CompactTag::MmapSlice)
            {
                if (!bytes.has_value())
                {
                    continue;
                }
                return *bytes;
            }
            // An owned string without a source reads as an empty cell, as GetValue gives it.
            if (!bytes.has_value())
            {
                return {};
            }
            // Stream bytes can be evicted later, so the caller gets a copy in its own buffer.
            buffer.assign(*bytes);
            return buffer;
        }
        const LogValue value = slot->Materialise(line.Source(), line.LineId(), id);
        if (std::holds_alternative<std::monostate>(value))
        {
            continue;
        }
        if (const auto *sv = std::get_if<std::string_view>(&value); sv != nullptr)
        {
            return *sv;
        }
        buffer.clear();
        AppendFormattedValue(buffer, columns[column].printFormat, value);
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

void LogTable::SetClock(std::function<std::chrono::system_clock::time_point()> clock)
{
    mClock = std::move(clock);
}

std::chrono::system_clock::time_point LogTable::ReadClock() const
{
    return mClock ? mClock() : std::chrono::system_clock::now();
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
            source->SetEnumDictionaries(&mEnum->Dictionaries());
        }
    }
}

const EnumDictionaryRegistry &LogTable::EnumDictionaries() const noexcept
{
    return mEnum->Dictionaries();
}

void LogTable::SetEnumValueCap(uint16_t cap) noexcept
{
    mEnum->SetValueCap(cap);
}

uint16_t LogTable::EnumValueCap() const noexcept
{
    return mEnum->ValueCap();
}

void LogTable::SetEnumValueMaxLen(uint32_t maxLen) noexcept
{
    mEnum->SetValueMaxLen(maxLen);
}

uint32_t LogTable::EnumValueMaxLen() const noexcept
{
    return mEnum->ValueMaxLen();
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
    return {.canonicalKey = canonicalKey, .dictionary = mEnum->Dictionaries().Find(canonicalKey)};
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
    return mEnum->RescanColumn(mData, mConfiguration, mColumnKeyIds, columnIndex);
}

LogTable::FinalizeResult LogTable::FinalizeAutoDetection()
{
    FinalizeResult result;
    (void)mEnum->TakeDemotedLevelValues();

    std::vector<ColumnType> typesBefore;
    {
        const auto &columnsBefore = mConfiguration.Configuration().columns;
        typesBefore.reserve(columnsBefore.size());
        for (const Column &column : columnsBefore)
        {
            typesBefore.push_back(column.type);
        }
    }

    result.promotedToEnumeration = mEnum->Finalize(mData, mConfiguration);
    const std::vector<KeyId> levelBubbleKeys = mEnum->TakePendingLevelBubbleKeys();
    result.errors = mEnum->TakePendingErrors();

    auto demotedValues = mEnum->TakeDemotedLevelValues();
    const auto &columnsAfter = mConfiguration.Configuration().columns;
    for (size_t columnIndex = 0; columnIndex < typesBefore.size() && columnIndex < columnsAfter.size(); ++columnIndex)
    {
        const ColumnType before = typesBefore[columnIndex];
        const ColumnType after = columnsAfter[columnIndex].type;
        if ((!IsEnumLike(before) && IsEnumLike(after)) ||
            (before == ColumnType::Enumeration && after == ColumnType::Level))
        {
            result.enumEvents.push_back({.column = columnIndex, .change = EnumColumnChange::Promoted});
        }
        else if (IsEnumLike(before) && !IsEnumLike(after))
        {
            EnumColumnEvent event{.column = columnIndex, .change = EnumColumnChange::Demoted};
            if (before == ColumnType::Level)
            {
                const KeyId key = CanonicalKeyOf(mData.Keys(), columnsAfter[columnIndex]);
                if (const auto values = demotedValues.find(key); values != demotedValues.end())
                {
                    event.levelValuesBeforeDemotion = std::move(values->second);
                }
            }
            result.enumEvents.push_back(std::move(event));
        }
    }
    for (const KeyId key : levelBubbleKeys)
    {
        if (const int columnIndex = FindColumnIndexByKey(key); columnIndex >= 0)
        {
            result.placements.push_back(
                {.kind = ColumnPlacement::Kind::LevelToCanonicalSlot, .column = static_cast<size_t>(columnIndex)}
            );
        }
    }
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
                mEnum->EraseTracker(canonical);
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
        mEnum->RefreshSnapshot(mData, mConfiguration);
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
        EnumColumnHealth &health = mEnum->HealthFor(canonical);
        const bool previousWasEnumLike = previousType == ColumnType::Enumeration || previousType == ColumnType::Level;
        if (!previousWasEnumLike)
        {
            health = EnumColumnHealth{};
        }
        if (!mEnum->EncodeColumnRangeAsEnum(
                mData, mConfiguration.Configuration().columns[columnIndex], 0U, mData.Lines().size(), health
            ))
        {
            mEnum->DemoteColumnFromEnum(mData, mConfiguration, columnIndex, /*recordForBatch=*/false);
            return;
        }
        if (column.type == ColumnType::Level)
        {
            mEnum->RefreshLevelRankCache(mData, mConfiguration.Configuration(), columnIndex);
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
        if (mEnum->Dictionaries().Find(canonical) != nullptr)
        {
            const auto targetType = column.type;
            mConfiguration.SetColumnType(columnIndex, ColumnType::Enumeration);
            mEnum->DemoteColumnFromEnum(mData, mConfiguration, columnIndex, /*recordForBatch=*/false);
            mConfiguration.SetColumnType(columnIndex, targetType);
        }
        mEnum->EraseHealthAndLevelCache(canonical);
        break;
    }
    }
}

std::optional<ColumnMove> LogTable::ResolvePlacement(const ColumnPlacement &placement) const
{
    const size_t from = placement.column;
    if (from >= mConfiguration.Configuration().columns.size())
    {
        return std::nullopt;
    }
    switch (placement.kind)
    {
    case ColumnPlacement::Kind::TimeToFront:
        if (from == 0)
        {
            return std::nullopt;
        }
        return ColumnMove{.from = from, .to = 0};
    case ColumnPlacement::Kind::LevelToCanonicalSlot:
        if (!ShouldBubbleLevelColumn(mConfiguration.Configuration(), from))
        {
            return std::nullopt;
        }
        return ColumnMove{.from = from, .to = CANONICAL_LEVEL_COLUMN_INDEX};
    }
    return std::nullopt;
}

void LogTable::ApplyPlacements(std::span<const ColumnPlacement> placements)
{
    std::vector<ColumnPlacement> pending(placements.begin(), placements.end());
    for (size_t i = 0; i < pending.size(); ++i)
    {
        if (const std::optional<ColumnMove> move = ResolvePlacement(pending[i]); move.has_value())
        {
            MoveColumn(move->from, move->to);
            for (size_t next = i + 1; next < pending.size(); ++next)
            {
                pending[next].column = move->Follow(pending[next].column);
            }
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
    const auto *ranks = mEnum->LevelRankCache(mData, mConfiguration.Configuration(), columnIndex);
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
    return mEnum->LevelRankCache(mData, mConfiguration.Configuration(), columnIndex);
}

std::string LogTable::FormatLogValue(const std::string &format, const LogValue &value) const
{
    std::string text;
    AppendFormattedValue(text, format, value);
    return text;
}

void LogTable::AppendFormattedValue(std::string &out, const std::string &format, const LogValue &value) const
{
    const std::size_t start = out.size();
    std::visit(
        [&](const auto &arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, std::string_view>)
            {
                out.append(arg);
            }
            else if constexpr (std::is_same_v<T, int64_t> || std::is_same_v<T, uint64_t>)
            {
                if (format == "{}")
                {
                    std::array<char, INTEGER_TEXT_CAPACITY> digits{};
                    const auto result = std::to_chars(digits.data(), digits.data() + digits.size(), arg);
                    out.append(digits.data(), result.ptr);
                    return;
                }
                AppendWithFmt(out, start, format, arg);
            }
            else if constexpr (std::is_same_v<T, double>)
            {
                if (const std::optional<int> precision = FixedPrecisionOf(format);
                    precision.has_value() && std::isfinite(arg))
                {
                    std::array<char, FIXED_DOUBLE_TEXT_CAPACITY> digits{};
                    const auto result = std::to_chars(
                        digits.data(), digits.data() + digits.size(), arg, std::chars_format::fixed, *precision
                    );
                    if (result.ec == std::errc{})
                    {
                        out.append(digits.data(), result.ptr);
                        return;
                    }
                }
                AppendWithFmt(out, start, format, arg);
            }
            else if constexpr (std::is_same_v<T, bool>)
            {
                AppendWithFmt(out, start, format, arg);
            }
            else if constexpr (std::is_same_v<T, TimeStamp>)
            {
                try
                {
                    mDisplayTimeZone.AppendFormatted(out, arg, format);
                }
                catch (const std::exception &)
                {
                    mDisplayTimeZone.AppendFormatted(out, arg, "%F %T");
                }
            }
            else if constexpr (std::is_same_v<T, std::monostate>)
            {
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
