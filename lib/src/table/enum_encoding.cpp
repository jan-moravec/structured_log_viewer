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

/// Microsecond threshold above which `DemoteColumnFromEnum` emits a
/// stderr telemetry line; below it the demote cost is uninteresting.
constexpr int64_t DEMOTE_TELEMETRY_LOG_THRESHOLD_US = 1000;

} // namespace

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

} // namespace loglib
