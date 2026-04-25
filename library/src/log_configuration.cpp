#include "loglib/log_configuration.hpp"

#include <glaze/glaze.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>

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

// Glaze 7.x moved indentation_width out of the core opts struct into an inheritable option.
struct PrettyOpts : glz::opts
{
    uint8_t indentation_width = 4;
};
constexpr PrettyOpts kPrettifyOpts{{.prettify = true}};

} // namespace

namespace loglib
{

void LogConfigurationManager::Load(const std::filesystem::path &path)
{
    std::ifstream file(path);
    if (file.is_open())
    {
        std::ostringstream buffer;
        buffer << file.rdbuf();
        const std::string content = buffer.str();
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
    const auto error = glz::write<kPrettifyOpts>(mConfiguration, json);
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
    // Update configuration columns with new keys. SortedKeys() snapshots the
    // KeyIndex into a std::vector<std::string> so this cold path does not need
    // to be aware of the new dense KeyId storage.
    for (const std::string &key : logData.SortedKeys())
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

void LogConfigurationManager::AppendKeys(const std::vector<std::string> &newKeys)
{
    // Streaming-mode column extension. Differs from Update in two ways:
    //   1. Operates on an explicit "new keys" slice (typically
    //      `StreamedBatch::newKeys`) rather than the canonical KeyIndex
    //      snapshot — avoids re-walking the entire SortedKeys list per batch.
    //   2. Always appends — never reorders. Timestamp auto-promotion still
    //      happens (so `LogTable::AppendBatch` knows which new columns need
    //      back-filling) but the new time column lands at the end alongside
    //      every other freshly-discovered key, preserving the append-only
    //      contract that Qt's `beginInsertColumns` relies on (PRD req.
    //      4.1.13 / Decision 14).
    for (const std::string &key : newKeys)
    {
        if (IsKeyInAnyColumn(key, mConfiguration.columns))
        {
            continue;
        }
        if (IsTimestampKey(key))
        {
            mConfiguration.columns.push_back(LogConfiguration::Column{
                key, {key}, "%F %H:%M:%S", LogConfiguration::Type::time, {"%FT%T%Ez", "%F %T%Ez", "%FT%T", "%F %T"}
            });
        }
        else
        {
            mConfiguration.columns.push_back(
                LogConfiguration::Column{key, {key}, "{}", LogConfiguration::Type::any, {}}
            );
        }
    }
}

const LogConfiguration &LogConfigurationManager::Configuration() const
{
    return mConfiguration;
}

} // namespace loglib
