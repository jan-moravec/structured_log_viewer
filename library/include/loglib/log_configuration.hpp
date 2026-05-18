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
    /// Per-column rendering / detection type. Auto-detection is gated
    /// by `Column::autoDetect`, not by `Type`: a column is an
    /// auto-detector candidate iff `type == Any && autoDetect`. Once a
    /// concrete type is set (by detection or the user), it is stable
    /// unless the user changes it.
    /// JSON wire format keeps the original lowerCamelCase keys
    /// (`"any"`, `"string"`, ...); see
    /// `internal/log_configuration_glaze_meta.hpp`.
    ///   - `Any`         - default for fresh keys (combined with
    ///                     `autoDetect = true`) and the auto-detect
    ///                     bail bucket for unclassifiable values (no
    ///                     values, or bool mixed with numeric). With
    ///                     `autoDetect = false` it is the explicit
    ///                     "treat as text, never auto-promote" opt-out.
    ///                     Sorts/filters as string.
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
        /// New keys default to `Type::Any` with `autoDetect = true`,
        /// which is the auto-detector candidate state. Time promotion
        /// is destructive (only `Reset()` reverts); enum promotion is
        /// reversible via demote-to-string on overflow (and only when
        /// `autoDetect == true`).
        Type type = Type::Any;
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
        /// When `true`, the auto-detector owns this column: a fresh
        /// `Type::Any` column will be scanned and promoted; an
        /// already-promoted `Enumeration`/`Level` column may still
        /// demote on overflow. When `false`, the user has pinned the
        /// column: no automatic promotion or demotion fires. Toggle
        /// via the UI's Column Editor; defaults to `true` so unedited
        /// columns retain the legacy "detector in charge" behaviour.
        bool autoDetect = true;
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

    /// Persisted sort state. `columnIndex = -1` is the inert default
    /// ("no sort applied"); positive indices reference `columns` and
    /// are remapped by `MoveColumn` alongside `filters[*].row`.
    struct Sort
    {
        int columnIndex = -1;
        bool descending = false;
    };

    /// Persisted source descriptor. `nullopt` means "no source bound"
    /// (a configuration-shape file). For session-shape files the app
    /// rebinds the source on load via this descriptor; rebind failure
    /// is non-fatal -- columns and filters still apply.
    struct Source
    {
        enum class Kind
        {
            File,
            NetworkStream
        };
        Kind kind = Kind::File;
        /// File path, network URI, or anything else the app can use
        /// to (re)open the source. Format is opaque to `loglib`; the
        /// app owns interpretation.
        std::string locator;
    };

    // Required field. Drives the column layout for every consumer.
    std::vector<Column> columns;

    // Session-only fields. Default-valued when absent on disk
    // (configuration-shape file); populated when a session save
    // includes them.
    std::vector<LogFilter> filters;
    Sort sort;
    std::optional<Source> source;
};

/// Case-insensitive match against known log-level field names (`level`,
/// `severity`, ...). `LogTable` uses this to gate `Enumeration -> Level`
/// promotion.
[[nodiscard]] bool IsLogLevelKey(const std::string &key);

/// Selects which `LogConfiguration` fields land on disk. Both kinds
/// share the same JSON schema, so a `ColumnsOnly` file is just a
/// `Full` file with the session-only members at their default values.
enum class SaveScope
{
    /// "Save Configuration\u2026": writes only `columns`. Filters, sort
    /// and source are omitted so the file is portable across data
    /// sources.
    ColumnsOnly,
    /// "Save Session\u2026": writes the full struct including filters,
    /// sort and (when present) source. Loaded session files
    /// re-establish the user's view state.
    Full
};

/// Loads, saves, and updates a `LogConfiguration` from observed data.
class LogConfigurationManager
{
public:
    LogConfigurationManager() = default;

    /// Throws `std::runtime_error` on open failure.
    void Load(const std::filesystem::path &path);
    /// Writes the full struct (equivalent to `Save(path, SaveScope::Full)`).
    void Save(const std::filesystem::path &path) const;
    /// Writes a subset of the struct selected by @p scope. Used by
    /// the UI's two save actions ("Save Configuration\u2026" / "Save
    /// Session\u2026").
    void Save(const std::filesystem::path &path, SaveScope scope) const;

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

    /// Toggle `Column::autoDetect` for @p columnIndex. No-op out of
    /// range. `true` puts the auto-detector in charge of the column
    /// (subject to the rules in the `Type` doc-comment); `false`
    /// pins the column at its current `type`.
    void SetColumnAutoDetect(size_t columnIndex, bool autoDetect);

    /// Toggle `Column::visible` for @p columnIndex. The column stays
    /// in the table; only the header section is hidden. No-op out
    /// of range.
    void SetColumnVisible(size_t columnIndex, bool visible);

    /// Replace `LogConfiguration::filters` wholesale. The app calls
    /// this from its `mFilters` -> wire-format mirror so `Save` and
    /// `MoveColumn`'s row remap see the live runtime set.
    void SetFilters(std::vector<LogConfiguration::LogFilter> filters);

    /// Replace `LogConfiguration::sort`. App calls this from its
    /// session-state mirror before a `Full` save so the persisted
    /// sort matches the user's current view.
    void SetSort(LogConfiguration::Sort sort);

    /// Replace `LogConfiguration::source`. App calls this from its
    /// session-state mirror; `nullopt` clears the binding.
    void SetSource(std::optional<LogConfiguration::Source> source);

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
