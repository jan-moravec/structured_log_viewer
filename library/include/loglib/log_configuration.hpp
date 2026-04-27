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

/// Manages the log configuration: loading, saving, updating from observed data.
class LogConfigurationManager
{
public:
    LogConfigurationManager() = default;

    /// Loads the configuration from @p path. Throws `std::runtime_error` if the
    /// file cannot be opened.
    void Load(const std::filesystem::path &path);

    /// Saves the configuration to @p path. Throws `std::runtime_error` if the
    /// file cannot be opened.
    void Save(const std::filesystem::path &path) const;

    /// Rebuilds the configuration from the current `LogData`. May reorder
    /// columns; not safe to call mid-stream.
    void Update(const LogData &logData);

    /// Append-only configuration extension used by the streaming path. Appends
    /// a column for any key in @p newKeys that is not already configured;
    /// auto-promotes timestamp-named keys to `Type::time`. Existing column
    /// indices stay put for the life of the parse so Qt's `beginInsertColumns`
    /// works correctly.
    void AppendKeys(const std::vector<std::string> &newKeys);

    const LogConfiguration &Configuration() const;

private:
    void EnsureKeyCacheBuilt() const;
    bool IsKeyInAnyColumnCached(const std::string &key) const;

    LogConfiguration mConfiguration;

    /// Cached set of every key that appears under any column's `keys` vector.
    /// Invalidation contract: every mutating path (`Load`, `Update`,
    /// `AppendKeys`) flips `mCacheStale`. Any future mutator must do the same.
    mutable std::unordered_set<std::string> mKeysInColumns;
    mutable bool mCacheStale = true;
};

} // namespace loglib
