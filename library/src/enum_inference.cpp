#include "loglib/enum_inference.hpp"

#include "loglib/compact_log_value.hpp"
#include "loglib/line_source.hpp"
#include "loglib/log_configuration.hpp"
#include "loglib/log_data.hpp"
#include "loglib/log_line.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace loglib
{

namespace
{

/// Microsecond threshold above which `DemoteColumnFromEnum` emits a
/// stderr telemetry line; below it the demote cost is uninteresting.
constexpr int64_t DEMOTE_TELEMETRY_LOG_THRESHOLD_US = 1000;

} // namespace

bool EnumColumnHealth::ShouldDemote(double tolerance, size_t minSamples) const noexcept
{
    if (totalSlots < minSamples)
    {
        return false;
    }
    const double bad = static_cast<double>(longValueSlots) + static_cast<double>(wrongTypeSlots);
    return bad > tolerance * static_cast<double>(totalSlots);
}

void EnumCandidateTracker::Observe(std::string_view bytes)
{
    if (killed)
    {
        return;
    }
    if (valueMaxLen != 0 && bytes.size() > valueMaxLen)
    {
        ++longValueCount;
        if (presenceCount >= ENUM_HEALTH_MIN_SAMPLES &&
            static_cast<double>(longValueCount) > ENUM_HEALTH_TOLERANCE_RATIO * static_cast<double>(presenceCount))
        {
            killed = true;
            values = {};
            seen = {};
            size = 0;
        }
        return;
    }
    if (seen.contains(bytes))
    {
        return;
    }
    if (size >= cap)
    {
        killed = true;
        values = {};
        seen = {};
        size = 0;
        return;
    }
    values.emplace_back(bytes);
    seen.emplace(values.back());
    ++size;
}

ColumnType RouteNoStringBail(
    size_t intObservations, size_t uintObservations, size_t doubleObservations, size_t boolObservations
) noexcept
{
    const bool sawIntegral = intObservations > 0 || uintObservations > 0;
    const bool sawDouble = doubleObservations > 0;
    const bool sawBool = boolObservations > 0;
    const bool sawNumeric = sawIntegral || sawDouble;
    if (sawBool && sawNumeric)
    {
        return ColumnType::Any;
    }
    if (sawBool)
    {
        return ColumnType::Boolean;
    }
    if (sawIntegral && sawDouble)
    {
        return ColumnType::Number;
    }
    if (sawIntegral)
    {
        return ColumnType::Integer;
    }
    if (sawDouble)
    {
        return ColumnType::Floating;
    }
    return ColumnType::Any;
}

bool IsEnumPassEligible(const Column &column) noexcept
{
    if (column.type == ColumnType::Enumeration || column.type == ColumnType::Level)
    {
        return true;
    }
    return column.type == ColumnType::Any && column.autoDetect;
}

void EnumInference::Clear()
{
    mDictionaries.Clear();
    mTrackers.clear();
    mColumnHealth.clear();
    mLevelRankCache.clear();
    mPendingLevelBubbleKeys.clear();
    mLastBatchDemotedKeys.clear();
    mIsStreaming = false;
}

void EnumInference::BeginStreaming()
{
    mDictionaries.Clear();
    mTrackers.clear();
    mColumnHealth.clear();
    mLevelRankCache.clear();
    mPendingLevelBubbleKeys.clear();
    mLastBatchDemotedKeys.clear();
    mIsStreaming = true;
}

void EnumInference::SetStreaming(bool streaming) noexcept
{
    mIsStreaming = streaming;
}

bool EnumInference::IsStreaming() const noexcept
{
    return mIsStreaming;
}

void EnumInference::SetValueCap(uint16_t cap) noexcept
{
    mValueCap = std::clamp<uint16_t>(cap, 1, MAX_ENUM_VALUES);
}

uint16_t EnumInference::ValueCap() const noexcept
{
    return mValueCap;
}

void EnumInference::SetValueMaxLen(uint32_t maxLen) noexcept
{
    mValueMaxLen = maxLen;
}

uint32_t EnumInference::ValueMaxLen() const noexcept
{
    return mValueMaxLen;
}

EnumDictionaryRegistry &EnumInference::Dictionaries() noexcept
{
    return mDictionaries;
}

const EnumDictionaryRegistry &EnumInference::Dictionaries() const noexcept
{
    return mDictionaries;
}

const std::vector<KeyId> &EnumInference::LastBatchDemotedKeys() const noexcept
{
    return mLastBatchDemotedKeys;
}

void EnumInference::ClearLastBatchDemotedKeys() noexcept
{
    mLastBatchDemotedKeys.clear();
}

std::vector<KeyId> EnumInference::TakePendingLevelBubbleKeys() noexcept
{
    return std::exchange(mPendingLevelBubbleKeys, std::vector<KeyId>{});
}

void EnumInference::EraseTracker(KeyId canonical)
{
    mTrackers.erase(canonical);
}

EnumColumnHealth &EnumInference::HealthFor(KeyId canonical)
{
    return mColumnHealth[canonical];
}

void EnumInference::EraseHealthAndLevelCache(KeyId canonical)
{
    mColumnHealth.erase(canonical);
    mLevelRankCache.erase(canonical);
}

void EnumInference::RefreshSnapshot(LogData &data, const LogConfigurationManager &configuration)
{
    mLevelRankCache.clear();
    const auto &columns = configuration.Configuration().columns;
    for (size_t columnIndex = 0; columnIndex < columns.size(); ++columnIndex)
    {
        const auto &column = columns[columnIndex];
        if (column.type != ColumnType::Enumeration && column.type != ColumnType::Level)
        {
            continue;
        }
        std::optional<KeyId> canonical;
        for (const std::string &key : column.keys)
        {
            const KeyId id = data.Keys().GetOrInsert(key);
            if (!canonical.has_value())
            {
                (void)mDictionaries.GetOrInsert(id, mValueCap);
                canonical = id;
            }
            else
            {
                if (!mDictionaries.Alias(*canonical, id))
                {
                    fmt::print(
                        stderr,
                        "[loglib] RefreshSnapshotEnumKeys: failed to alias key {} onto canonical {} for column "
                        "'{}'\n",
                        static_cast<uint32_t>(id),
                        static_cast<uint32_t>(*canonical),
                        column.header
                    );
                }
            }
        }
        if (column.type == ColumnType::Level)
        {
            RefreshLevelRankCache(data, configuration.Configuration(), columnIndex);
        }
    }
}

void EnumInference::RunPassForAppendBatch(
    LogData &data,
    LogConfigurationManager &configuration,
    const std::vector<std::vector<KeyId>> &columnKeyIds,
    size_t oldLineCount,
    std::optional<size_t> &firstBackfilled,
    std::optional<size_t> &lastBackfilled
)
{
    const auto &columns = configuration.Configuration().columns;
    const size_t totalRows = data.Lines().size();
    if (totalRows == 0 || oldLineCount >= totalRows)
    {
        return;
    }

    auto recordBackfill = [&](size_t columnIndex) {
        if (!firstBackfilled.has_value() || columnIndex < *firstBackfilled)
        {
            firstBackfilled = columnIndex;
        }
        if (!lastBackfilled.has_value() || columnIndex > *lastBackfilled)
        {
            lastBackfilled = columnIndex;
        }
    };

    std::vector<KeyId> resolvedKeys;
    auto resolveKeys = [&](size_t columnIndex) {
        resolvedKeys.clear();
        resolvedKeys.reserve(columnKeyIds[columnIndex].size());
        for (const KeyId id : columnKeyIds[columnIndex])
        {
            if (id != INVALID_KEY_ID)
            {
                resolvedKeys.push_back(id);
            }
        }
    };

    for (size_t columnIndex = 0; columnIndex < columns.size(); ++columnIndex)
    {
        const auto &column = columns[columnIndex];
        if (!IsEnumPassEligible(column))
        {
            continue;
        }
        if (columnIndex >= columnKeyIds.size())
        {
            continue;
        }

        if (column.type == ColumnType::Enumeration || column.type == ColumnType::Level)
        {
            resolveKeys(columnIndex);
            if (resolvedKeys.empty())
            {
                continue;
            }
            EnumColumnHealth &health = mColumnHealth[resolvedKeys.front()];

            const EnumDictionary *dictBefore = mDictionaries.Find(resolvedKeys.front());
            const size_t oldDictSize = (dictBefore != nullptr) ? dictBefore->Size() : 0;

            const bool encodeOk = EncodeColumnRange(data, resolvedKeys, oldLineCount, totalRows, health);
            if (!encodeOk)
            {
                if (column.autoDetect)
                {
                    DemoteColumnFromEnum(data, configuration, columnIndex);
                    recordBackfill(columnIndex);
                }
                continue;
            }
            if (column.autoDetect && health.ShouldDemote(ENUM_HEALTH_TOLERANCE_RATIO, ENUM_HEALTH_MIN_SAMPLES))
            {
                DemoteColumnFromEnum(data, configuration, columnIndex);
                recordBackfill(columnIndex);
                continue;
            }
            if (column.type == ColumnType::Enumeration && column.autoDetect)
            {
                const EnumDictionary *dictAfter = mDictionaries.Find(resolvedKeys.front());
                const size_t newDictSize = (dictAfter != nullptr) ? dictAfter->Size() : 0;
                if (newDictSize > oldDictSize && std::ranges::any_of(column.keys, IsLogLevelKey))
                {
                    MaybePromoteToLevel(data, configuration, columnIndex);
                }
            }
            const auto &columnAfterMaybe = columns[columnIndex];
            if (columnAfterMaybe.type == ColumnType::Level)
            {
                RefreshLevelRankCache(data, configuration.Configuration(), columnIndex);
            }
            continue;
        }

        if (column.keys.empty())
        {
            continue;
        }

        const size_t promotionMinRows = mIsStreaming ? STREAM_PROMOTION_MIN_ROWS : ENUM_PROMOTION_MIN_ROWS;

        resolveKeys(columnIndex);
        if (resolvedKeys.empty())
        {
            continue;
        }
        const KeyId trackerKey = resolvedKeys.front();

        auto trackerIt = mTrackers.find(trackerKey);
        if (trackerIt == mTrackers.end())
        {
            trackerIt = mTrackers.emplace(trackerKey, EnumCandidateTracker{mValueCap, mValueMaxLen}).first;
        }
        EnumCandidateTracker &tracker = trackerIt->second;

        const size_t scanCap = 2 * promotionMinRows;
        for (size_t row = oldLineCount; row < totalRows; ++row)
        {
            const LogLine &line = data.Lines()[row];
            const CompactLogValue *slot = nullptr;
            for (const KeyId id : resolvedKeys)
            {
                slot = line.FindCompact(id);
                if (slot != nullptr)
                {
                    break;
                }
            }
            ++tracker.rowsObserved;
            if (slot != nullptr)
            {
                ++tracker.presenceCount;
                std::optional<std::string_view> bytes = line.PeekStringView(*slot);
                if (bytes.has_value())
                {
                    tracker.Observe(*bytes);
                }
                else if (slot->tag == CompactTag::Int64)
                {
                    ++tracker.intObservations;
                }
                else if (slot->tag == CompactTag::Uint64)
                {
                    ++tracker.uintObservations;
                }
                else if (slot->tag == CompactTag::Double)
                {
                    ++tracker.doubleObservations;
                }
                else if (slot->tag == CompactTag::Bool)
                {
                    ++tracker.boolObservations;
                }
            }
            if (tracker.killed)
            {
                break;
            }
            if (tracker.size > 0 && tracker.size <= mValueCap && tracker.presenceCount >= promotionMinRows)
            {
                break;
            }
            if (tracker.rowsObserved >= scanCap)
            {
                break;
            }
        }

        if (tracker.killed)
        {
            configuration.SetColumnType(columnIndex, ColumnType::String);
            mTrackers.erase(trackerIt);
            continue;
        }

        if (tracker.size > 0 && tracker.size <= mValueCap && tracker.presenceCount >= promotionMinRows)
        {
            PromoteColumnToEnum(data, configuration, columnIndex);
            mTrackers.erase(trackerIt);
            recordBackfill(columnIndex);
            continue;
        }

        if (tracker.rowsObserved >= scanCap)
        {
            if (tracker.presenceCount == 0)
            {
                tracker.rowsObserved = 0;
                continue;
            }
            const bool noStringSeen = tracker.size == 0;
            const bool highCardinality = !mIsStreaming && tracker.size > 0 &&
                                         static_cast<double>(tracker.size) >
                                             ENUM_CARDINALITY_BAIL_RATIO * static_cast<double>(tracker.presenceCount);
            if (noStringSeen)
            {
                configuration.SetColumnType(
                    columnIndex,
                    RouteNoStringBail(
                        tracker.intObservations,
                        tracker.uintObservations,
                        tracker.doubleObservations,
                        tracker.boolObservations
                    )
                );
                mTrackers.erase(trackerIt);
            }
            else if (highCardinality)
            {
                configuration.SetColumnType(columnIndex, ColumnType::String);
                mTrackers.erase(trackerIt);
            }
        }
    }
}

ColumnType EnumInference::RescanColumn(
    LogData &data,
    LogConfigurationManager &configuration,
    const std::vector<std::vector<KeyId>> &columnKeyIds,
    size_t columnIndex
)
{
    const auto &columns = configuration.Configuration().columns;
    if (columnIndex >= columns.size())
    {
        return ColumnType::Any;
    }
    {
        const auto &column = columns[columnIndex];
        if (column.type != ColumnType::Any || !column.autoDetect || column.keys.empty())
        {
            return column.type;
        }
    }
    const size_t totalRows = data.Lines().size();
    if (totalRows == 0 || columnIndex >= columnKeyIds.size())
    {
        return ColumnType::Any;
    }

    std::vector<KeyId> resolvedKeys;
    resolvedKeys.reserve(columnKeyIds[columnIndex].size());
    for (const KeyId id : columnKeyIds[columnIndex])
    {
        if (id != INVALID_KEY_ID)
        {
            resolvedKeys.push_back(id);
        }
    }
    if (resolvedKeys.empty())
    {
        return ColumnType::Any;
    }

    const KeyId trackerKey = resolvedKeys.front();
    mTrackers.erase(trackerKey);

    EnumCandidateTracker tracker{mValueCap, mValueMaxLen};
    for (size_t row = 0; row < totalRows; ++row)
    {
        const LogLine &line = data.Lines()[row];
        const CompactLogValue *slot = nullptr;
        for (const KeyId id : resolvedKeys)
        {
            slot = line.FindCompact(id);
            if (slot != nullptr)
            {
                break;
            }
        }
        ++tracker.rowsObserved;
        if (slot != nullptr)
        {
            ++tracker.presenceCount;
            std::optional<std::string_view> bytes = line.PeekStringView(*slot);
            if (bytes.has_value())
            {
                tracker.Observe(*bytes);
            }
            else if (slot->tag == CompactTag::Int64)
            {
                ++tracker.intObservations;
            }
            else if (slot->tag == CompactTag::Uint64)
            {
                ++tracker.uintObservations;
            }
            else if (slot->tag == CompactTag::Double)
            {
                ++tracker.doubleObservations;
            }
            else if (slot->tag == CompactTag::Bool)
            {
                ++tracker.boolObservations;
            }
        }
        if (tracker.killed)
        {
            break;
        }
    }

    if (tracker.killed)
    {
        configuration.SetColumnType(columnIndex, ColumnType::String);
    }
    else if (tracker.size > 0 && tracker.size <= mValueCap && tracker.presenceCount >= 2)
    {
        PromoteColumnToEnum(data, configuration, columnIndex);
    }
    else if (tracker.size == 0 && tracker.presenceCount > 0)
    {
        configuration.SetColumnType(
            columnIndex,
            RouteNoStringBail(
                tracker.intObservations, tracker.uintObservations, tracker.doubleObservations, tracker.boolObservations
            )
        );
    }
    return configuration.Configuration().columns[columnIndex].type;
}

bool EnumInference::Finalize(LogData &data, LogConfigurationManager &configuration)
{
    bool promoted = false;
    const auto &columns = configuration.Configuration().columns;
    if (!mTrackers.empty())
    {
        for (size_t columnIndex = 0; columnIndex < columns.size(); ++columnIndex)
        {
            const auto &column = columns[columnIndex];
            if (column.type != ColumnType::Any || !column.autoDetect || column.keys.empty())
            {
                continue;
            }
            const KeyId trackerKey = data.Keys().Find(column.keys.front());
            if (trackerKey == INVALID_KEY_ID)
            {
                continue;
            }
            auto trackerIt = mTrackers.find(trackerKey);
            if (trackerIt == mTrackers.end())
            {
                continue;
            }
            const EnumCandidateTracker &tracker = trackerIt->second;
            if (tracker.killed)
            {
                configuration.SetColumnType(columnIndex, ColumnType::String);
                continue;
            }
            if (tracker.size > 0 && tracker.size <= mValueCap && tracker.presenceCount >= 2)
            {
                PromoteColumnToEnum(data, configuration, columnIndex);
                promoted = true;
                continue;
            }
            if (tracker.size == 0 && tracker.presenceCount > 0)
            {
                configuration.SetColumnType(
                    columnIndex,
                    RouteNoStringBail(
                        tracker.intObservations,
                        tracker.uintObservations,
                        tracker.doubleObservations,
                        tracker.boolObservations
                    )
                );
                continue;
            }
        }
    }

    {
        const auto &columnsForDemote = configuration.Configuration().columns;
        for (size_t columnIndex = 0; columnIndex < columnsForDemote.size(); ++columnIndex)
        {
            const auto &column = columnsForDemote[columnIndex];
            if ((column.type != ColumnType::Enumeration && column.type != ColumnType::Level) || !column.autoDetect ||
                column.keys.empty())
            {
                continue;
            }
            const KeyId canonical = data.Keys().Find(column.keys.front());
            if (canonical == INVALID_KEY_ID)
            {
                continue;
            }
            const auto healthIt = mColumnHealth.find(canonical);
            if (healthIt == mColumnHealth.end())
            {
                continue;
            }
            const EnumColumnHealth &health = healthIt->second;
            if (health.totalSlots == 0)
            {
                continue;
            }
            if (health.ShouldDemote(ENUM_HEALTH_TOLERANCE_RATIO, /*minSamples=*/1U))
            {
                DemoteColumnFromEnum(data, configuration, columnIndex, /*recordForBatch=*/false);
            }
        }
    }

    mTrackers.clear();
    mIsStreaming = false;
    return promoted;
}

bool EnumInference::EncodeColumnRange(
    LogData &data, std::span<const KeyId> aliasKeys, size_t rowBegin, size_t rowEnd, EnumColumnHealth &health
)
{
    if (aliasKeys.empty())
    {
        return true;
    }
    EnumDictionary *dict = nullptr;
    {
        EnumDictionary &ref = mDictionaries.GetOrInsert(aliasKeys.front(), mValueCap);
        dict = &ref;
    }
    auto &lines = data.Lines();
    for (size_t row = rowBegin; row < rowEnd && row < lines.size(); ++row)
    {
        LogLine &line = lines[row];
        bool encoded = false;
        bool sawLong = false;
        bool sawWrongType = false;
        bool alreadyEncoded = false;
        for (const KeyId id : aliasKeys)
        {
            CompactLogValue *slot = line.FindCompactMutable(id);
            if (slot == nullptr)
            {
                continue;
            }
            if (slot->tag == CompactTag::DictRef)
            {
                alreadyEncoded = true;
                break;
            }
            const auto bytes = line.PeekStringView(*slot);
            if (!bytes.has_value())
            {
                sawWrongType = true;
                continue;
            }
            if (mValueMaxLen != 0 && bytes->size() > mValueMaxLen)
            {
                sawLong = true;
                continue;
            }
            const EnumValueId vid = dict->Insert(*bytes);
            if (vid == INVALID_ENUM_VALUE_ID)
            {
                return false;
            }
            *slot = CompactLogValue::MakeDictRef(vid);
            encoded = true;
            break;
        }
        if (alreadyEncoded)
        {
            continue;
        }
        if (encoded)
        {
            ++health.totalSlots;
        }
        else if (sawLong)
        {
            ++health.totalSlots;
            ++health.longValueSlots;
        }
        else if (sawWrongType)
        {
            ++health.totalSlots;
            ++health.wrongTypeSlots;
        }
    }
    return true;
}

bool EnumInference::EncodeColumnRangeAsEnum(
    LogData &data, const Column &column, size_t rowBegin, size_t rowEnd, EnumColumnHealth &health
)
{
    std::vector<KeyId> keyIds;
    keyIds.reserve(column.keys.size());
    for (const std::string &key : column.keys)
    {
        const KeyId id = data.Keys().Find(key);
        if (id != INVALID_KEY_ID)
        {
            keyIds.push_back(id);
        }
    }
    if (keyIds.empty())
    {
        return true;
    }
    return EncodeColumnRange(data, keyIds, rowBegin, rowEnd, health);
}

void EnumInference::PromoteColumnToEnum(LogData &data, LogConfigurationManager &configuration, size_t columnIndex)
{
    if (columnIndex >= configuration.Configuration().columns.size())
    {
        return;
    }
    {
        const auto &snapshot = configuration.Configuration().columns[columnIndex];
        if (snapshot.type == ColumnType::Enumeration || snapshot.type == ColumnType::Level)
        {
            return;
        }
    }

    const std::vector<std::string> columnKeys = configuration.Configuration().columns[columnIndex].keys;
    const std::string headerKey = configuration.Configuration().columns[columnIndex].header;

    configuration.SetColumnType(columnIndex, ColumnType::Enumeration);
    KeyId canonicalKey = INVALID_KEY_ID;
    {
        std::optional<KeyId> canonical;
        for (const std::string &key : columnKeys)
        {
            const KeyId id = data.Keys().GetOrInsert(key);
            if (!canonical.has_value())
            {
                (void)mDictionaries.GetOrInsert(id, mValueCap);
                canonical = id;
            }
            else
            {
                if (!mDictionaries.Alias(*canonical, id))
                {
                    fmt::print(
                        stderr,
                        "[loglib] PromoteColumnToEnum: failed to alias key {} onto canonical {} for column '{}'\n",
                        static_cast<uint32_t>(id),
                        static_cast<uint32_t>(*canonical),
                        headerKey
                    );
                }
            }
        }
        if (canonical.has_value())
        {
            canonicalKey = *canonical;
        }
    }

    EnumColumnHealth &health = mColumnHealth[canonicalKey];
    if (!EncodeColumnRangeAsEnum(
            data, configuration.Configuration().columns[columnIndex], 0U, data.Lines().size(), health
        ))
    {
        DemoteColumnFromEnum(data, configuration, columnIndex);
        return;
    }
    if (health.ShouldDemote(ENUM_HEALTH_TOLERANCE_RATIO, ENUM_HEALTH_MIN_SAMPLES))
    {
        DemoteColumnFromEnum(data, configuration, columnIndex);
        return;
    }

    if (std::ranges::any_of(columnKeys, IsLogLevelKey))
    {
        MaybePromoteToLevel(data, configuration, columnIndex);
    }
}

void EnumInference::DemoteColumnFromEnum(
    LogData &data, LogConfigurationManager &configuration, size_t columnIndex, bool recordForBatch
)
{
    const auto &columns = configuration.Configuration().columns;
    if (columnIndex >= columns.size())
    {
        return;
    }
    const auto &column = columns[columnIndex];
    if (column.type != ColumnType::Enumeration && column.type != ColumnType::Level)
    {
        return;
    }

    std::vector<KeyId> keyIds;
    keyIds.reserve(column.keys.size());
    for (const std::string &key : column.keys)
    {
        const KeyId id = data.Keys().Find(key);
        if (id != INVALID_KEY_ID)
        {
            keyIds.push_back(id);
        }
    }

    if (recordForBatch && !keyIds.empty())
    {
        mLastBatchDemotedKeys.push_back(keyIds.front());
    }

    auto &lines = data.Lines();

    const auto demoteStart = std::chrono::steady_clock::now();
    size_t convertedSlots = 0;

    if (!keyIds.empty())
    {
        const EnumDictionary *dict = mDictionaries.Find(keyIds.front());
        for (auto &line : lines)
        {
            LineSource *source = line.Source();
            const size_t lineId = line.LineId();
            for (const KeyId id : keyIds)
            {
                CompactLogValue *slot = line.FindCompactMutable(id);
                if (slot == nullptr || slot->tag != CompactTag::DictRef)
                {
                    continue;
                }
                std::string_view bytes;
                if (dict != nullptr)
                {
                    bytes = dict->Resolve(static_cast<EnumValueId>(slot->payload));
                }
                if (source != nullptr)
                {
                    const uint64_t offset = source->AppendOwnedBytes(lineId, bytes);
                    *slot = CompactLogValue::MakeOwnedString(offset, static_cast<uint32_t>(bytes.size()));
                }
                else
                {
                    *slot = CompactLogValue::MakeMonostate();
                }
                ++convertedSlots;
            }
        }
        mDictionaries.Erase(keyIds.front());
    }

    configuration.SetColumnType(columnIndex, ColumnType::String);

    if (!keyIds.empty())
    {
        mColumnHealth.erase(keyIds.front());
        mLevelRankCache.erase(keyIds.front());
    }

    const auto demoteElapsed =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - demoteStart);
    if (demoteElapsed.count() > DEMOTE_TELEMETRY_LOG_THRESHOLD_US)
    {
        fmt::print(
            stderr,
            "[loglib] DemoteColumnFromEnum column={} rows={} slots={} elapsed={}us\n",
            columnIndex,
            lines.size(),
            convertedSlots,
            demoteElapsed.count()
        );
    }
}

void EnumInference::MaybePromoteToLevel(LogData &data, LogConfigurationManager &configuration, size_t columnIndex)
{
    if (columnIndex >= configuration.Configuration().columns.size())
    {
        return;
    }
    const auto snapshotColumn = [&]() -> const Column & {
        return configuration.Configuration().columns[columnIndex];
    };
    if (snapshotColumn().type != ColumnType::Enumeration)
    {
        return;
    }
    if (!snapshotColumn().autoDetect)
    {
        return;
    }

    if (!std::ranges::any_of(snapshotColumn().keys, IsLogLevelKey))
    {
        return;
    }

    KeyId canonical = INVALID_KEY_ID;
    for (const std::string &key : snapshotColumn().keys)
    {
        canonical = data.Keys().Find(key);
        if (canonical != INVALID_KEY_ID)
        {
            break;
        }
    }
    if (canonical == INVALID_KEY_ID)
    {
        return;
    }
    const EnumDictionary *dict = mDictionaries.Find(canonical);
    if (dict == nullptr || dict->Size() == 0)
    {
        return;
    }

    size_t canonicalEntries = 0;
    size_t unrecognizedEntries = 0;
    {
        const auto &column = snapshotColumn();
        for (size_t valueId = 0; valueId < dict->Size(); ++valueId)
        {
            const std::string_view bytes = dict->Resolve(static_cast<EnumValueId>(valueId));
            if (ResolveLevel(bytes, column.levelMapping).has_value())
            {
                ++canonicalEntries;
            }
            else
            {
                ++unrecognizedEntries;
            }
        }
    }
    if (canonicalEntries == 0 || unrecognizedEntries * LEVEL_DICT_TOLERANCE_RATIO > canonicalEntries)
    {
        return;
    }

    configuration.SetColumnType(columnIndex, ColumnType::Level);
    RefreshLevelRankCache(data, configuration.Configuration(), columnIndex);
    mPendingLevelBubbleKeys.push_back(canonical);
}

void EnumInference::RefreshLevelRankCache(
    const LogData &data, const LogConfiguration &configuration, size_t columnIndex
)
{
    const auto &columns = configuration.columns;
    if (columnIndex >= columns.size())
    {
        return;
    }
    const auto &column = columns[columnIndex];
    if (column.type != ColumnType::Level)
    {
        return;
    }

    KeyId canonical = INVALID_KEY_ID;
    for (const std::string &key : column.keys)
    {
        canonical = data.Keys().Find(key);
        if (canonical != INVALID_KEY_ID)
        {
            break;
        }
    }
    if (canonical == INVALID_KEY_ID)
    {
        return;
    }
    const EnumDictionary *dict = mDictionaries.Find(canonical);
    if (dict == nullptr)
    {
        mLevelRankCache[canonical] = {};
        return;
    }

    std::vector<LogLevel> &ranks = mLevelRankCache[canonical];
    if (ranks.size() > dict->Size())
    {
        ranks.clear();
    }
    ranks.reserve(dict->Size());
    for (size_t valueId = ranks.size(); valueId < dict->Size(); ++valueId)
    {
        const std::string_view bytes = dict->Resolve(static_cast<EnumValueId>(valueId));
        const std::optional<LogLevel> level = ResolveLevel(bytes, column.levelMapping);
        ranks.push_back(level.value_or(LogLevel::Unknown));
    }
}

const std::vector<LogLevel> *EnumInference::LevelRankCache(
    const LogData &data, const LogConfiguration &configuration, size_t columnIndex
) const noexcept
{
    const auto &columns = configuration.columns;
    if (columnIndex >= columns.size())
    {
        return nullptr;
    }
    const auto &column = columns[columnIndex];
    if (column.type != ColumnType::Level || column.keys.empty())
    {
        return nullptr;
    }
    const KeyId canonical = data.Keys().Find(column.keys.front());
    if (canonical == INVALID_KEY_ID)
    {
        return nullptr;
    }
    const auto it = mLevelRankCache.find(canonical);
    if (it == mLevelRankCache.end())
    {
        return nullptr;
    }
    return &it->second;
}

} // namespace loglib
