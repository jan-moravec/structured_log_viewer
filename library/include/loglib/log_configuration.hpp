#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace loglib
{

// Forward-declared to avoid pulling the full `log_data.hpp` chain
// (`log_file.hpp` / `key_index.hpp` / `log_line.hpp` and their robin_map
// instantiations) into every consumer of this header. TUs that use
// `LogData` directly include `log_data.hpp` themselves.
class LogData;

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
        /// Per-cell rendering type.
        ///
        /// - `Type::any` — render the underlying `LogValue` variant
        ///   verbatim via `printFormat`.
        /// - `Type::time` — pre-parse into a `TimeStamp` so sorting and
        ///   the time-range filter work numerically.
        ///
        /// **`Type::time` promotion is destructive**, in two ways:
        ///   1. Stage B replaces the per-line `LogValue` with the parsed
        ///      `TimeStamp` in place; the original string is gone.
        ///   2. The streaming path auto-flips a `Type::any` column to
        ///      `Type::time` when a newly-seen key matches the timestamp
        ///      heuristic (`timestamp`, `time`, `ts`, …). Already-promoted
        ///      rows can't be reverted without `LogTable::Reset()`.
        ///
        /// Invisible to GUI users (the round-tripped print form matches
        /// the input), but visible to programmatic consumers that diff a
        /// saved configuration against the in-memory one.
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

    /// Append-only extension used by the streaming path: appends any key
    /// in @p newKeys not already configured, auto-promoting timestamp-
    /// named keys (`timestamp`, `time`, `ts`, `@timestamp`, …) to
    /// `Type::time`. Existing column indices stay put so Qt's
    /// `beginInsertColumns` stays valid. **Auto-promotion is destructive
    /// on `Column::type`** — see `LogConfiguration::Column::type`.
    void AppendKeys(const std::vector<std::string> &newKeys);

    /// Non-mutating count of @p newKeys an `AppendKeys` call would add as
    /// fresh columns. Used by `LogTable::PreviewAppend` so
    /// `LogModel::AppendBatch` can fire `beginInsertColumns` before the
    /// underlying mutation (Qt's begin-before-mutate contract).
    size_t CountAppendableKeys(const std::vector<std::string> &newKeys) const;

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
