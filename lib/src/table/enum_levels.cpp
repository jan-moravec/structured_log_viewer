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

void EnumInference::MaybePromoteToLevel(LogData &data, LogConfigurationManager &configuration, size_t columnIndex)
{
    if (columnIndex >= configuration.Configuration().columns.size())
    {
        return;
    }
    const auto snapshotColumn = [&] -> const Column & { return configuration.Configuration().columns[columnIndex]; };
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

    const KeyId canonical = CanonicalKeyOf(data.Keys(), snapshotColumn());
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

    const KeyId canonical = CanonicalKeyOf(data.Keys(), column);
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
    if (column.type != ColumnType::Level)
    {
        return nullptr;
    }
    const KeyId canonical = CanonicalKeyOf(data.Keys(), column);
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
