#pragma once

#include "log_data.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace loglib
{

struct LogConfiguration
{
    enum class Type
    {
        any,
        time
    };

    struct Column
    {
        std::string header;
        std::vector<std::string> keys;
        std::string printFormat;
        Type type = Type::any;
        std::vector<std::string> parseFormats;
    };

    struct LogFilter
    {
        enum class Type
        {
            string,
            time
        };

        enum class Match
        {
            exactly,
            contains,
            regularExpression,
            wildcard
        };

        Type type;
        int row;
        std::optional<std::string> filterString;
        std::optional<Match> matchType;
        std::optional<int64_t> filterBegin;
        std::optional<int64_t> filterEnd;
    };

    std::vector<Column> columns;
    std::vector<LogFilter> filters;
};

/// Manages the log configuration: loading, saving, and updating from
/// observed data.
class LogConfigurationManager
{
public:
    LogConfigurationManager() = default;

    /// Throws `std::runtime_error` if the file cannot be opened.
    void Load(const std::filesystem::path &path);
    void Save(const std::filesystem::path &path) const;

    /// Rebuilds the configuration from @p logData. Not safe to call mid-stream.
    void Update(const LogData &logData);

    /// Append-only extension used by the streaming path: appends any key in
    /// @p newKeys not already configured, auto-promoting timestamp-named keys
    /// to `Type::time`. Existing column indices stay put for the life of the
    /// parse so Qt's `beginInsertColumns` stays valid.
    void AppendKeys(const std::vector<std::string> &newKeys);

    const LogConfiguration &Configuration() const;

private:
    void EnsureKeyCacheBuilt() const;
    bool IsKeyInAnyColumnCached(const std::string &key) const;

    LogConfiguration mConfiguration;

    /// Cached "every key referenced by any column". Every mutator must flip
    /// `mCacheStale`.
    mutable std::unordered_set<std::string> mKeysInColumns;
    mutable bool mCacheStale = true;
};

} // namespace loglib
