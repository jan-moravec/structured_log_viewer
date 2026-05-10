#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace loglib
{

// Forward-declared so consumers don't pull in the full `log_data.hpp` chain.
class LogData;

struct LogConfiguration
{
    /// Per-column rendering / detection type. `unknown` is the only
    /// state the auto-detector scans; every other variant is terminal.
    ///   - `unknown`     - new keys; auto-detector candidate.
    ///   - `any`         - mixed / unclassifiable.
    ///   - `string`      - too varied to enumerate.
    ///   - `integer`     - only Int64/UInt64 observed.
    ///   - `floating`    - only Double observed.
    ///   - `number`      - mix of integer and floating.
    ///   - `time`        - timestamp column.
    ///   - `enumeration` - small fixed vocabulary stored as `DictRef`.
    enum class Type
    {
        unknown,
        any,
        string,
        integer,
        floating,
        number,
        time,
        enumeration
    };

    struct Column
    {
        std::string header;
        std::vector<std::string> keys;
        std::string printFormat;
        /// New keys default to `Type::unknown`. Time promotion is
        /// destructive (only `Reset()` reverts); enum promotion is
        /// reversible via demote-to-string on overflow.
        Type type = Type::unknown;
        std::vector<std::string> parseFormats;
    };

    struct LogFilter
    {
        enum class Type
        {
            string,
            time,
            /// Multi-select over an enum column. Persisted as strings,
            /// resolved to an id bitset at rule construction.
            enumeration
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
        /// Selected values for `Type::enumeration`. Empty otherwise.
        std::vector<std::string> filterValues;
    };

    std::vector<Column> columns;
    std::vector<LogFilter> filters;
};

/// Loads, saves, and updates a `LogConfiguration` from observed data.
class LogConfigurationManager
{
public:
    LogConfigurationManager() = default;

    /// Throws `std::runtime_error` on open failure.
    void Load(const std::filesystem::path &path);
    void Save(const std::filesystem::path &path) const;

    /// Rebuilds the configuration from @p logData. Not safe mid-stream.
    void Update(const LogData &logData);

    /// Append-only: adds keys not already configured, auto-promoting
    /// timestamp-named ones. Existing column indices stay put.
    void AppendKeys(const std::vector<std::string> &newKeys);

    /// Non-mutating count of fresh columns `AppendKeys(newKeys)` would add.
    size_t CountAppendableKeys(const std::vector<std::string> &newKeys) const;

    /// Move the column at @p srcIndex to @p destIndex.
    void MoveColumn(size_t srcIndex, size_t destIndex);

    /// Flip the type of the column at @p columnIndex; caller back-fills
    /// any row data. No-op out of range.
    void SetColumnType(size_t columnIndex, LogConfiguration::Type type);

    const LogConfiguration &Configuration() const;

private:
    void EnsureKeyCacheBuilt() const;
    bool IsKeyInAnyColumnCached(const std::string &key) const;

    LogConfiguration mConfiguration;

    /// Cached "every key referenced by any column"; mutators flip
    /// `mCacheStale`.
    mutable std::unordered_set<std::string> mKeysInColumns;
    mutable bool mCacheStale = true;
};

} // namespace loglib
