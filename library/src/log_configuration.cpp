#include "loglib/log_configuration.hpp"

#include <algorithm>
#include <fstream>

namespace
{

bool IsKeyInAnyColumn(const std::string &key, const std::vector<loglib::LogConfiguration::Column> &columns)
{
    for (const auto &column : columns)
    {
        if (std::find(column.keys.begin(), column.keys.end(), key) != column.keys.end())
        {
            return true;
        }
    }

    return false;
}

std::string ToLower(const std::string &str)
{
    std::string lower = str;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](auto c) {
        return static_cast<unsigned char>(std::tolower(c));
    });
    return lower;
}

bool IsTimestampKey(const std::string &key)
{
    static const std::vector<std::string> TIMESTAMP_KEYS = {"timestamp", "time", "t"};
    return std::any_of(
        TIMESTAMP_KEYS.begin(),
        TIMESTAMP_KEYS.end(),
        [lowerKey = ToLower(key)](const std::string &value) { return (lowerKey == value); }
    );
}

} // namespace

namespace loglib
{

void LogConfigurationManager::Load(const std::filesystem::path &path)
{
    nlohmann::json json;
    std::ifstream file(path);
    if (file.is_open())
    {
        file >> json;
    }
    else
    {
        throw std::runtime_error("Failed to open file '" + path.string() + "'.");
    }

    mConfiguration = json.get<LogConfiguration>();
}

void LogConfigurationManager::Save(const std::filesystem::path &path) const
{
    nlohmann::json json = mConfiguration;
    std::ofstream file(path);
    if (file.is_open())
    {
        file << json.dump(4);
    }
    else
    {
        throw std::runtime_error("Failed to open file '" + path.string() + "'.");
    }
}

void LogConfigurationManager::Update(const LogData &logData)
{
    // Update configuration columns with new keys
    for (const std::string &key : logData.Keys())
    {
        if (!IsKeyInAnyColumn(key, mConfiguration.columns))
        {
            if (IsTimestampKey(key))
            {
                mConfiguration.columns.push_back(LogConfiguration::Column{
                    key, {key}, "%F %H:%M:%S", LogConfiguration::Type::Time, {"%FT%T%Ez", "%F %T%Ez", "%FT%T", "%F %T"}
                });
                // Timestamp should be the first column, all the others will be shifted
                for (size_t i = mConfiguration.columns.size() - 1; i > 0; --i)
                {
                    std::swap(mConfiguration.columns[i], mConfiguration.columns[i - 1]);
                }
            }
            else
            {
                mConfiguration.columns.push_back(
                    LogConfiguration::Column{key, {key}, "{}", LogConfiguration::Type::Any, {}}
                );
            }
        }
    }
}

const LogConfiguration &LogConfigurationManager::Configuration() const
{
    return mConfiguration;
}

} // namespace loglib
