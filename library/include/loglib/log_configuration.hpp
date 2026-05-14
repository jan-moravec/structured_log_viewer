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
        Enumeration
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
        /// keep working); only `QHeaderView::setSectionHidden` is
        /// toggled in the view. Defaults to `true` so legacy JSON
        /// (missing key) loads as visible.
        bool visible = true;
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

        /// Defaults chosen so a default-constructed `LogFilter` is
        /// inert: `row = -1` is rejected by `OutOfRangeRow` in
        /// `ValidateFilterAgainstColumns`, and `String` is the most
        /// permissive type (no missing-payload obligations beyond
        /// `filterString` / `matchType`, which the validator catches).
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

    /// Move the column at @p srcIndex to @p destIndex. Also remaps
    /// every `LogConfiguration::filters[*].row` through the same
    /// single-column permutation so persisted filters follow the
    /// column they originally pointed at.
    void MoveColumn(size_t srcIndex, size_t destIndex);

    /// Flip the type of the column at @p columnIndex; caller back-fills
    /// any row data. No-op out of range.
    void SetColumnType(size_t columnIndex, LogConfiguration::Type type);

    /// Toggle visibility of @p columnIndex; no-op out of range. The
    /// column stays in the table -- only `Column::visible` flips, so
    /// data, sort, and filters keep working. View applies the flag
    /// via `QHeaderView::setSectionHidden`.
    void SetColumnVisible(size_t columnIndex, bool visible);

    /// Replace the configuration's filter vector wholesale. Intended
    /// for the app's eager mirror of `MainWindow::mFilters` -> the
    /// wire-format `LogConfiguration::filters`, so that `Save` and
    /// the lib-side `MoveColumn` filter-row remap operate on the
    /// live runtime set rather than a permanently empty vector.
    void SetFilters(std::vector<LogConfiguration::LogFilter> filters);

    /// Apply the single-column reorder permutation `(srcIndex ->
    /// destIndex)` to a stored column index @p columnIndex (e.g. a
    /// `LogFilter::row`). Returns the new column index; out-of-range
    /// inputs pass through unchanged. Exposed so the app's live
    /// in-memory filter map can be remapped with the same logic the
    /// manager uses internally.
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
