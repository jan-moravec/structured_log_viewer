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
    /// Per-column rendering / detection type.
    ///
    /// `unknown` is the only type the auto-detector scans; every
    /// other variant is terminal (the detector skips it). Newly-
    /// discovered keys default to `unknown`; the kill paths in
    /// `LogTable::RunEnumPassForAppendBatch` route them to the
    /// appropriate terminal based on what was observed.
    ///
    /// Provenance summary:
    /// - `unknown`     - default for new keys; auto-detector candidate.
    /// - `any`         - terminal: mixed / unclassifiable (e.g. all
    ///                    nulls or bools, or a mix of value kinds).
    /// - `string`      - terminal: strings too varied to enumerate
    ///                    (cardinality bail / length cap / dict cap /
    ///                    overflow demote).
    /// - `integer`     - terminal: only Int64 / UInt64 observed.
    /// - `floating`    - terminal: only Double observed. The C++
    ///                    identifier mirrors `std::is_floating_point`
    ///                    (the keyword `double` cannot be used as an
    ///                    enumerator); glaze serialises it as the
    ///                    JSON string "floating" — the wire and
    ///                    source spellings are kept in lockstep.
    /// - `number`      - terminal: a mix of integer and floating values.
    /// - `time`        - terminal: name-matched timestamp column.
    /// - `enumeration` - terminal: small fixed string vocabulary.
    ///
    /// `string`, `integer`, `floating`, and `number` currently fall
    /// through the same default branches as `any` for filter UI
    /// and rendering. Numeric sort, integer-vs-double formatting,
    /// and range-filter widgets are deferred to follow-up PRs.
    enum class Type
    {
        unknown,
        any,
        string,
        integer,
        floating,
        number,
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
        /// Per-cell rendering type. See `Type` for the variant
        /// summary. Newly-discovered keys default to `Type::unknown`.
        ///
        /// **Time promotion is destructive**: Stage B replaces the
        /// per-line `LogValue` with a parsed `TimeStamp` in place; only
        /// `LogTable::Reset()` reverts.
        ///
        /// **Enum promotion is destructive but reversible**: when a
        /// new value would push the dictionary past the cap,
        /// `LogTable::AppendBatch` demotes the column to
        /// `Type::string` and rewrites every slot to `OwnedString`.
        Type type = Type::unknown;
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
