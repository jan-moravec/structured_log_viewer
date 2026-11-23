#include "loglib/parse/internal/parse_runtime.hpp"

#include "loglib/time/internal/timestamp_parsing.hpp"
#include "loglib/time/time_zone_context.hpp"

#include <algorithm>
#include <span>
#include <string>
#include <utility>

namespace loglib::internal
{

TimeConversion BuildTimeConversion(
    KeyIndex &keys,
    std::span<const Column> columns,
    std::span<const KeyId> declaredKeys,
    const TimeZoneContext &timeZone,
    std::chrono::system_clock::time_point referenceTime
)
{
    TimeConversion conversion;
    AutomaticTimeKeys &automatic = conversion.automatic;
    for (const Column &column : columns)
    {
        for (const std::string &key : column.keys)
        {
            automatic.excluded.insert(key);
        }
        if (column.type != ColumnType::Time)
        {
            continue;
        }
        TimeColumnSpec spec;
        spec.timeZone = timeZone;
        spec.referenceTime = referenceTime;
        spec.keyIds.reserve(column.keys.size());
        for (const std::string &key : column.keys)
        {
            spec.keyIds.push_back(keys.GetOrInsert(key));
        }
        spec.formats.reserve(column.parseFormats.size());
        for (const std::string &format : column.parseFormats)
        {
            spec.formats.emplace_back(format);
        }
        conversion.specs.push_back(std::move(spec));
    }

    automatic.prototype.timeZone = timeZone;
    automatic.prototype.referenceTime = referenceTime;
    for (const std::string &format : DefaultTimeParseFormats())
    {
        automatic.prototype.formats.emplace_back(format);
    }
    for (const KeyId id : declaredKeys)
    {
        const std::string key(keys.KeyOf(id));
        if (!automatic.Matches(key))
        {
            continue;
        }
        TimeColumnSpec spec = automatic.prototype;
        spec.keyIds = {id};
        conversion.specs.push_back(std::move(spec));
        automatic.excluded.insert(key);
    }
    return conversion;
}

std::vector<KeyId> ConvertedTimeKeys(std::span<const TimeColumnSpec> specs)
{
    std::vector<KeyId> keys;
    for (const TimeColumnSpec &spec : specs)
    {
        keys.insert(keys.end(), spec.keyIds.begin(), spec.keyIds.end());
    }
    std::ranges::sort(keys);
    keys.erase(std::ranges::unique(keys).begin(), keys.end());
    return keys;
}

} // namespace loglib::internal
