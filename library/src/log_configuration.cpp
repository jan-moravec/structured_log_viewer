#include "loglib/log_configuration.hpp"

#include <glaze/glaze.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace
{

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
        // Configuration was replaced wholesale; the next `IsKeyInAnyColumn` query
        // rebuilds the cache against the loaded columns.
        mCacheStale = true;
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
    EnsureKeyCacheBuilt();
    for (const std::string &key : logData.SortedKeys())
    {
        if (!IsKeyInAnyColumnCached(key))
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
            // Keep the cache consistent inside the loop so a `SortedKeys()` snapshot that
            // contains the same key twice (currently impossible — `KeyIndex` dedupes — but
            // robust against future callers) does not double-add the column.
            mKeysInColumns.insert(key);
        }
    }
}

void LogConfigurationManager::AppendKeys(const std::vector<std::string> &newKeys)
{
    // Streaming-mode column extension. Differs from `Update` in two ways:
    //   1. Operates on an explicit "new keys" slice (`StreamedBatch::newKeys`)
    //      rather than re-walking the full canonical KeyIndex.
    //   2. Always appends — never reorders. Timestamp auto-promotion still
    //      happens, but the new time column lands at the end alongside every
    //      other freshly-discovered key, preserving the append-only contract
    //      that Qt's `beginInsertColumns` relies on.
    EnsureKeyCacheBuilt();
    for (const std::string &key : newKeys)
    {
        if (IsKeyInAnyColumnCached(key))
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
        // Same in-loop cache maintenance as `Update`: a duplicate key in `newKeys`
        // (legal — `StreamedBatch::newKeys` is unique per batch but two consecutive
        // batches' slices may overlap if the caller forwards them naively) must not
        // add two columns for the same key.
        mKeysInColumns.insert(key);
    }
}

const LogConfiguration &LogConfigurationManager::Configuration() const
{
    return mConfiguration;
}

void LogConfigurationManager::EnsureKeyCacheBuilt() const
{
    if (!mCacheStale)
    {
        return;
    }
    mKeysInColumns.clear();
    mKeysInColumns.reserve(mConfiguration.columns.size() * 4);
    for (const LogConfiguration::Column &column : mConfiguration.columns)
    {
        for (const std::string &key : column.keys)
        {
            mKeysInColumns.insert(key);
        }
    }
    mCacheStale = false;
}

bool LogConfigurationManager::IsKeyInAnyColumnCached(const std::string &key) const
{
    EnsureKeyCacheBuilt();
    return mKeysInColumns.find(key) != mKeysInColumns.end();
}

} // namespace loglib
