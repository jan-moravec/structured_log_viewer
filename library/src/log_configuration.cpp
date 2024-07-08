#include "log_configuration.hpp"

#include <algorithm>
#include <fstream>

namespace loglib
{

bool IsKeyInAnyColumn(const std::string& key, const std::vector<LogConfiguration::Column> &columns) {
    for (const auto &column: columns)
    {
        if (std::find(column.keys.begin(), column.keys.end(), key) != column.keys.end())
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
                configuration.columns.push_back(LogConfiguration::Column{key, {key}, "%F %H:%M:%S", LogConfiguration::Type::Time, {"%FT%T%Ez", "%F %T%Ez", "%FT%T", "%F %T"}});
                // Timestamp should be the first column, all the others will be shifted
                for (size_t i = configuration.columns.size() - 1; i > 0; --i)
                {
                    std::swap(configuration.columns[i], configuration.columns[i - 1]);
                }
            }
            else
            {
                configuration.columns.push_back(LogConfiguration::Column{key, {key}, "{}"});
            }
        }
    }
}

void SerializeConfiguration(const std::filesystem::path &path, const LogConfiguration &configuration)
{
    nlohmann::json json = configuration;
    std::ofstream file(path);
    if (file.is_open())
    {
        file <<  json.dump(4);
    }
    else
    {
        throw std::runtime_error("Failed to open file " + path.string());
    }
}

LogConfiguration DeserializeConfiguration(const std::filesystem::path &path)
{
    nlohmann::json json;
    std::ifstream file(path);
    if (file.is_open())
    {
        file >>  json;
    }
    else
    {
        throw std::runtime_error("Failed to open file " + path.string());
    }

    return json.get<LogConfiguration>();
}

}
