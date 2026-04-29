#include "loglib/log_configuration.hpp"

#include "loglib/log_data.hpp"

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

// Glaze 7.x: indentation_width moved off of glz::opts into an inheritable option.
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
                // Timestamps land in the first column; shift everything else right.
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
            mKeysInColumns.insert(key);
        }
    }
}

void LogConfigurationManager::AppendKeys(const std::vector<std::string> &newKeys)
{
    // Always append — never reorder — to keep Qt's `beginInsertColumns` valid.
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
            mConfiguration.columns.push_back(LogConfiguration::Column{key, {key}, "{}", LogConfiguration::Type::any, {}}
            );
        }
        mKeysInColumns.insert(key);
    }
}

size_t LogConfigurationManager::CountAppendableKeys(const std::vector<std::string> &newKeys) const
{
    if (newKeys.empty())
    {
        return 0;
    }
    EnsureKeyCacheBuilt();
    // Mirror `AppendKeys`'s skip predicate (`IsKeyInAnyColumnCached`) plus
    // duplicates within @p newKeys itself: the streaming harness only emits
    // freshly-seen keys per batch, but a defensive de-dupe here keeps this
    // helper safe to call on any input. Allocation-free for the common case.
    std::unordered_set<std::string_view> alreadyCounted;
    alreadyCounted.reserve(newKeys.size());
    size_t count = 0;
    for (const std::string &key : newKeys)
    {
        if (IsKeyInAnyColumnCached(key))
        {
            continue;
        }
        if (alreadyCounted.insert(std::string_view(key)).second)
        {
            ++count;
        }
    }
    return count;
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
