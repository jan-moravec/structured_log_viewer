#include "loglib/parse/internal/parse_runtime.hpp"

#include "loglib/time/time_zone_context.hpp"
#include "loglib/time/timestamp_parsing.hpp"

#include <span>
#include <string>
#include <utility>

namespace loglib::internal
{

std::vector<TimeColumnSpec> BuildTimeColumnSpecs(
    KeyIndex &keys,
    std::span<const Column> columns,
    const TimeZoneContext &timeZone,
    std::chrono::system_clock::time_point referenceTime
)
{
    std::vector<TimeColumnSpec> result;
    for (const Column &column : columns)
    {
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
        spec.parseFormats = column.parseFormats;
        spec.formatKinds.reserve(spec.parseFormats.size());
        for (const std::string &format : spec.parseFormats)
        {
            spec.formatKinds.push_back(ClassifyTimestampFormat(format));
        }
        result.push_back(std::move(spec));
    }
    return result;
}

} // namespace loglib::internal
