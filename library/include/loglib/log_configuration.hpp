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
        /// Column whose values are drawn from a small set
        /// (<= `loglib::MAX_ENUM_VALUES`) and stored as
        /// `CompactTag::DictRef` after auto-detection. Promoted /
        /// demoted by `LogTable::AppendBatch`'s enum pass; never
        /// auto-promoted by the timestamp-name heuristic.
        enumeration
    };

    struct Column
    {
        std::string header;
        std::vector<std::string> keys;
        std::string printFormat;
        /// Per-cell rendering type. `Type::any` renders via `printFormat`;
        /// `Type::time` pre-parses into a `TimeStamp` for numeric sort/filter;
        /// `Type::enumeration` stores values as dictionary references for
        /// memory savings and a value-picker filter UX.
        ///
        /// **`Type::time` promotion is destructive**: Stage B replaces the
        /// per-line `LogValue` with the parsed `TimeStamp` in place, and the
        /// streaming path auto-flips `Type::any` → `Type::time` for keys
        /// matching the timestamp heuristic. Only `LogTable::Reset()` reverts.
        ///
        /// `Type::enumeration` promotion is also destructive but reversible:
        /// when a 17th distinct value would arrive, `LogTable::AppendBatch`
        /// demotes the column back to `Type::any` and rewrites every slot
        /// from `DictRef` to `OwnedString`.
        Type type = Type::any;
        std::vector<std::string> parseFormats;
    };

    struct LogFilter
    {
        enum class Type
        {
            string,
            time,
            /// Multi-select picker over the column's
            /// `EnumDictionary::Values()`. Persisted as a list of
            /// strings (`filterValues`); resolved to `EnumValueId`s at
            /// rule construction time so filtering is one `uint16`
            /// compare per row.
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
    /// `LogTable`'s enum auto-detector for promotion (`any` ->
    /// `enumeration`) and demotion (`enumeration` -> `any`). Caller is
    /// responsible for any matching backfill of the row data;
    /// `LogConfigurationManager` only tracks the configuration value.
    /// No-op when @p columnIndex is out of range.
    void SetColumnType(size_t columnIndex, LogConfiguration::Type type);

    /// Returns true iff @p canonicalKey was added by `Update` or
    /// `AppendKeys` (i.e. discovered from streamed/observed data) and
    /// has not since been overridden by a `Load`. Used by the enum
    /// auto-detector to lock columns whose `Type::any` came from a
    /// loaded configuration: such columns are not eligible for
    /// auto-promotion to `Type::enumeration`. Demotion is unaffected
    /// (a configured `Type::enumeration` column that overflows the
    /// cap demotes regardless of this flag and stays demoted via
    /// `LogTable::mEnumPermanentlyKilled`).
    ///
    /// The set is transient runtime state; it is not serialised by
    /// `Save`. A `Save` followed by `Load` round-trip therefore drops
    /// the auto-discovered status, which is the explicit semantic of
    /// the lock: persisting `Type::any` in a saved config means the
    /// user has accepted that column as `any`, so a future session
    /// must not re-promote it without further data-driven discovery.
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

    /// Set of canonical column keys (`column.keys.front()`) that were
    /// added by data-driven discovery -- `Update` (synchronous parse)
    /// or `AppendKeys` (streaming) -- and have not since been
    /// overridden by `Load`. Read by `IsAutoDiscoveredColumn`.
    /// Transient: not serialised; cleared by `Load`. See
    /// `IsAutoDiscoveredColumn` for the locking semantic.
    std::unordered_set<std::string> mAutoDiscoveredCanonicalKeys;
};

} // namespace loglib
