#include "log_configuration.hpp"

#include <algorithm>

namespace loglib
{

bool IsKeyInAnyColumn(const std::string& key, const std::unordered_map<size_t, LogConfiguration::Column> &columns) {
    for (const auto &column: columns)
    {
        if (std::find(column.second.keys.begin(), column.second.keys.end(), key) != column.second.keys.end())
        {
            return true;
        }
    }

    return false;
}

std::string ToLower(const std::string& str) {
    std::string lower = str;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    return lower;
}

bool IsTimestampKey(const std::string& key)
{
    static const std::vector<std::string> TIMESTAMP_KEYS = {"timestamp", "time", "t"};
    return std::any_of(TIMESTAMP_KEYS.begin(), TIMESTAMP_KEYS.end(), [lowerKey = ToLower(key)](const std::string& value) {
        return (lowerKey == value);
    });
}

void UpdateConfiguration(LogConfiguration &configuration, const LogData &logData)
{
    // Update configuration columns with new keys
    for (const std::string &key: logData.GetKeys())
    {
        if (!IsKeyInAnyColumn(key, configuration.columns))
        {
            if (IsTimestampKey(key))
            {
                const size_t size = configuration.columns.size();
                configuration.columns[size] = LogConfiguration::Column{key, {key}, "%F %H:%M:%S", LogConfiguration::Type::Time, {"%FT%T%Ez", "%F %T%Ez", "%FT%T", "%F %T"}};
                // Timestamp should be the first column, all the others will be shifted
                for (size_t i = size; i > 0; --i)
                {
                    std::swap(configuration.columns[i], configuration.columns[i - 1]);
                }
            }
            else
            {
                configuration.columns[configuration.columns.size()] = LogConfiguration::Column{key, {key}, "{}"};
            }
        }
    }
}

}
