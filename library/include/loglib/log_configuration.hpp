#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace loglib
{

// Forward-declared so consumers don't pull in the full `log_data.hpp` chain.
class LogData;

struct LogConfiguration
{
    /// Per-column rendering / detection type. `Unknown` is the only
    /// state the auto-detector scans; every other variant is terminal.
    /// JSON wire format keeps the original lowerCamelCase keys
    /// (`"unknown"`, `"any"`, ...); see
    /// `internal/log_configuration_glaze_meta.hpp`.
    ///   - `Unknown`     - new keys; auto-detector candidate.
    ///   - `Any`         - user opt-out, or auto-detect bail for
    ///                     unclassifiable values (no values, or bool
    ///                     mixed with numeric). Sorts/filters as string.
    ///   - `String`      - inferred string column (too varied to enumerate).
    ///   - `Boolean`     - JSON `true`/`false` slots; false < true.
    ///   - `Integer`     - only Int64/UInt64 observed.
    ///   - `Floating`    - only Double observed.
    ///   - `Number`      - mix of integer and floating.
    ///   - `Time`        - timestamp column.
    ///   - `Enumeration` - small fixed vocabulary stored as `DictRef`.
    ///   - `Level`       - Enumeration subtype for log-level columns.
    ///                     Raw strings stay in the dictionary; a
    ///                     per-column cache maps each id to a canonical
    ///                     `LogLevel` so sort/filter/styling use
    ///                     severity rank instead of alphabetic order.
    enum class Type
    {
        Unknown,
        Any,
        String,
        Boolean,
        Integer,
        Floating,
        Number,
        Time,
        Enumeration,
        Level
    };

    struct Column
    {
        std::string header;
        std::vector<std::string> keys;
        std::string printFormat;
        /// New keys default to `Type::Unknown`. Time promotion is
        /// destructive (only `Reset()` reverts); enum promotion is
        /// reversible via demote-to-string on overflow.
        Type type = Type::Unknown;
        std::vector<std::string> parseFormats;
        /// Hidden columns stay in the table (data, sort, and filters
        /// keep working); only the view toggles `setSectionHidden`.
        /// Defaults to `true` so legacy JSON loads as visible.
        bool visible = true;
        /// Per-column alias overrides for `Type::Level` columns. Each
        /// entry is `(alias, canonicalName)`: aliases match the raw
        /// user string case-insensitively, canonical names must spell
        /// a `LogLevel` (`"Info"`, `"Warn"`, ...). Augments the
        /// built-in alias table. Ignored for non-Level columns.
        std::vector<std::pair<std::string, std::string>> levelMapping;
    };

    struct LogFilter
    {
        enum class Type
        {
            String,
            Time,
            /// Multi-select over an enum column. Persisted as strings,
            /// resolved to an id bitset at rule construction.
            Enumeration,
            /// Inclusive numeric range over `Integer`/`Floating`/`Number`
            /// columns. Carried in `filterMinValue`/`filterMaxValue`
            /// (either may be `nullopt` for unbounded).
            Number,
            /// True/false multi-select for `Type::Boolean` columns.
            /// `filterValues` is a subset of {"true","false"}; empty
            /// rejects every row.
            Boolean
        };

        enum class Match
        {
            Exactly,
            Contains,
            RegularExpression,
            Wildcard
        };

        /// Defaults make a default-constructed `LogFilter` inert:
        /// `row = -1` is rejected by `ValidateFilterAgainstColumns`,
        /// and `String` is the most permissive type.
        Type type = Type::String;
        int row = -1;
        std::optional<std::string> filterString;
        std::optional<Match> matchType;
        std::optional<int64_t> filterBegin;
        std::optional<int64_t> filterEnd;
        /// Inclusive lower bound for `Type::Number`. `nullopt` means
        /// unbounded (-inf).
        std::optional<double> filterMinValue;
        /// Inclusive upper bound for `Type::Number`. `nullopt` means
        /// unbounded (+inf).
        std::optional<double> filterMaxValue;
        /// Selected values for `Type::Enumeration` and `Type::Boolean`.
        /// Empty otherwise.
        std::vector<std::string> filterValues;
    };

    std::vector<Column> columns;
    std::vector<LogFilter> filters;
};

/// Case-insensitive match against known log-level field names (`level`,
/// `severity`, ...). `LogTable` uses this to gate `Enumeration -> Level`
/// promotion.
[[nodiscard]] bool IsLogLevelKey(const std::string &key);

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

    /// Move the column at @p srcIndex to @p destIndex. Also runs
    /// every `LogConfiguration::filters[*].row` through the same
    /// permutation so persisted filters follow the column.
    void MoveColumn(size_t srcIndex, size_t destIndex);

    /// Flip the type of the column at @p columnIndex; caller
    /// back-fills row data. No-op out of range.
    void SetColumnType(size_t columnIndex, LogConfiguration::Type type);

    /// Toggle `Column::visible` for @p columnIndex. The column stays
    /// in the table; only the header section is hidden. No-op out
    /// of range.
    void SetColumnVisible(size_t columnIndex, bool visible);

    /// Replace `LogConfiguration::filters` wholesale. The app calls
    /// this from its `mFilters` -> wire-format mirror so `Save` and
    /// `MoveColumn`'s row remap see the live runtime set.
    void SetFilters(std::vector<LogConfiguration::LogFilter> filters);

    /// Apply the `(srcIndex -> destIndex)` single-column move to a
    /// stored column index. Out-of-range inputs pass through
    /// unchanged. Exposed so the app can remap its runtime filter
    /// map with the same logic.
    [[nodiscard]] static int RemapColumnIndexAfterMove(int columnIndex, int srcIndex, int destIndex);

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
