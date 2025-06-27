#include "loglib/log_configuration.hpp"

#include <glaze/glaze.hpp>

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
    std::ifstream file(path);
    if (file.is_open())
    {
        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        const auto error = glz::read_json(mConfiguration, content);
        if (error)
        {
            throw std::runtime_error("Failed to parse configuration file: " + glz::format_error(error, content));
        }
    }
    else
    {
        throw std::runtime_error("Failed to open file '" + path.string() + "'.");
    }
}

void LogConfigurationManager::Save(const std::filesystem::path &path) const
{
    std::string json;
    const auto error = glz::write<glz::opts{.prettify = true, .indentation_width = 4}>(mConfiguration, json);
    if (error)
    {
        throw std::runtime_error("Failed to serialize configuration: " + glz::format_error(error));
    }

    std::ofstream file(path);
    if (file.is_open())
    {
        file << json;
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
                    key, {key}, "%F %H:%M:%S", LogConfiguration::Type::time, {"%FT%T%Ez", "%F %T%Ez", "%FT%T", "%F %T"}
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
                    LogConfiguration::Column{key, {key}, "{}", LogConfiguration::Type::any, {}}
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
