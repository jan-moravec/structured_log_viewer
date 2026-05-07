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
        time,
        /// Column with a small set of distinct string values
        /// (<= `MAX_ENUM_VALUES`), stored as `CompactTag::DictRef`.
        /// Promoted/demoted by `LogTable`'s enum pass.
        enumeration
    };

    struct Column
    {
        std::string header;
        std::vector<std::string> keys;
        std::string printFormat;
        /// Per-cell rendering type. `Type::any` renders via `printFormat`;
        /// `Type::time` pre-parses into a `TimeStamp`;
        /// `Type::enumeration` stores values as dictionary references.
        ///
        /// **Time promotion is destructive**: Stage B replaces the
        /// per-line `LogValue` with a parsed `TimeStamp` in place; only
        /// `LogTable::Reset()` reverts.
        ///
        /// **Enum promotion is destructive but reversible**: when a
        /// new value would push the dictionary past the cap,
        /// `LogTable::AppendBatch` demotes the column back to
        /// `Type::any` and rewrites every slot to `OwnedString`.
        Type type = Type::any;
        std::vector<std::string> parseFormats;
    };

    struct LogFilter
    {
        enum class Type
        {
            string,
            time,
            /// Multi-select over an enum column's dictionary values.
            /// Persisted as `filterValues` strings; resolved to a
            /// bitset of `EnumValueId`s at rule construction.
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
        /// Selected dictionary values for `Type::enumeration` filters.
        /// Empty for the other filter types.
        std::vector<std::string> filterValues;
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

    /// Append-only extension used by the streaming path: appends keys not
    /// already configured, auto-promoting timestamp-named keys (`timestamp`,
    /// `time`, `ts`, `@timestamp`, …) to `Type::time`. Existing column
    /// indices stay put. \see `LogConfiguration::Column::type` for the
    /// destructive-promotion contract.
    void AppendKeys(const std::vector<std::string> &newKeys);

    /// Non-mutating count of fresh columns `AppendKeys(newKeys)` would add.
    /// Used by `LogTable::PreviewAppend` for Qt's begin-before-mutate contract.
    size_t CountAppendableKeys(const std::vector<std::string> &newKeys) const;

    /// Moves the column at @p srcIndex to @p destIndex, shifting the
    /// intermediate columns by one slot. Used by the streaming path to
    /// promote freshly-discovered timestamp columns to the front (matching
    /// the `Update` synchronous-parse behaviour) once Qt has been informed
    /// via `beginMoveColumns`. Indices must be in `[0, columns.size())`.
    void MoveColumn(size_t srcIndex, size_t destIndex);

    /// Flip the `Type` of the column at @p columnIndex. Used by
    /// the enum auto-detector. Caller backfills any row data; this
    /// only updates the configuration. No-op out of range.
    void SetColumnType(size_t columnIndex, LogConfiguration::Type type);

    /// True iff @p canonicalKey was added by `Update` or `AppendKeys`
    /// and not since overridden by `Load`. The enum auto-detector uses
    /// this to lock columns whose `Type::any` came from a saved
    /// configuration; persisting `Type::any` is treated as the user
    /// accepting it. Not serialised.
    [[nodiscard]] bool IsAutoDiscoveredColumn(const std::string &canonicalKey) const;

    const LogConfiguration &Configuration() const;

private:
    void EnsureKeyCacheBuilt() const;
    bool IsKeyInAnyColumnCached(const std::string &key) const;

    LogConfiguration mConfiguration;

    /// Cached "every key referenced by any column". Every mutator must flip
    /// `mCacheStale`.
    mutable std::unordered_set<std::string> mKeysInColumns;
    mutable bool mCacheStale = true;

    /// Canonical keys (`column.keys.front()`) added by data-driven
    /// discovery (`Update` / `AppendKeys`). Cleared by `Load`. Not
    /// serialised. Read by `IsAutoDiscoveredColumn`.
    std::unordered_set<std::string> mAutoDiscoveredCanonicalKeys;
};

} // namespace loglib
