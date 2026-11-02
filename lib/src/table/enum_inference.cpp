#include "loglib/table/enum_inference.hpp"

#include "loglib/core/compact_log_value.hpp"
#include "loglib/core/line_source.hpp"
#include "loglib/core/log_data.hpp"
#include "loglib/core/log_line.hpp"
#include "loglib/schema/log_configuration.hpp"

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

/// Whether a finished column's values repeat enough to be an enumeration.
/// Log levels qualify however few rows the file has.
bool ValuesRepeatOrAreLevels(const EnumCandidateTracker &tracker, const Column &column)
{
    return tracker.size * 2U <= tracker.presenceCount || std::ranges::any_of(column.keys, IsLogLevelKey);
}

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
    else if (
        tracker.size > 0 && tracker.size <= mValueCap && tracker.presenceCount >= 2 &&
        ValuesRepeatOrAreLevels(tracker, columns[columnIndex])
    )
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
            if (tracker.size > 0 && tracker.size <= mValueCap && tracker.presenceCount >= 2 &&
                ValuesRepeatOrAreLevels(tracker, column))
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

} // namespace loglib
