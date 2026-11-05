#include "loglib/parse/internal/parse_runtime.hpp"

#include "loglib/time/time_zone_context.hpp"
#include "loglib/time/timestamp_parsing.hpp"

#include <string>
#include <utility>

namespace loglib::internal
{

std::vector<TimeColumnSpec> BuildTimeColumnSpecs(KeyIndex &keys, const LogConfiguration *configuration)
{
    std::vector<TimeColumnSpec> result;
    if (configuration == nullptr)
    {
        return result;
    }
    const TimeZoneContext timeZone = ProcessDefaultTimeZone();
    for (const Column &column : configuration->columns)
    {
        if (column.type != ColumnType::Time)
        {
            continue;
        }
        TimeColumnSpec spec;
        spec.timeZone = timeZone;
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
