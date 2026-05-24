#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
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
    /// by `Column::autoDetect`: a column is a detector candidate iff
    /// `type == Any && autoDetect`. JSON wire format uses lowerCamelCase
    /// keys; see `internal/log_configuration_glaze_meta.hpp`.
    ///   - `Any`         - default for fresh keys (with `autoDetect`)
    ///                     and the bail bucket for unclassifiable
    ///                     values. With `autoDetect=false`, the
    ///                     explicit "treat as text" opt-out. Sorts /
    ///                     filters as string.
    ///   - `String`      - inferred string column.
    ///   - `Boolean`     - JSON `true`/`false`; false < true.
    ///   - `Integer`     - only Int64/UInt64 observed.
    ///   - `Floating`    - only Double observed.
    ///   - `Number`      - mix of integer and floating.
    ///   - `Time`        - timestamp column.
    ///   - `Enumeration` - small fixed vocabulary stored as `DictRef`.
    ///   - `Level`       - Enumeration subtype for log-level columns;
    ///                     sorts / filters / styles by severity rank.
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
        /// Defaults to the detector-candidate state (paired with
        /// `autoDetect=true` below). Time promotion is destructive
        /// (only `Reset()` reverts); enum promotion can demote on
        /// overflow, but only while `autoDetect` is on.
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
        /// `true`: the auto-detector owns the column (scans `Any`
        /// candidates, demotes overflowing enums). `false`: the user
        /// has pinned the column; no automatic promotion or demotion.
        /// Defaults to `true` so unedited columns keep the legacy
        /// behaviour.
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

    /// Persisted sort. `columnIndex == -1` means "no sort applied";
    /// positive indices index `columns` and are remapped by
    /// `MoveColumn` alongside `filters[*].row`.
    struct Sort
    {
        int columnIndex = -1;
        bool descending = false;
    };

    /// Persisted source descriptor. `nullopt` means "no source bound".
    /// On load the app may re-open this; rebind failure is non-fatal
    /// (columns and filters still apply). The non-fatal promise also
    /// covers schema drift: legacy session JSON that used the pre-
    /// widening `"locator":"..."` shape loses its source binding on
    /// load but keeps columns / filters / sort, courtesy of the
    /// `error_on_unknown_keys=false` option configured for the read
    /// path (`loglib/internal/log_configuration_glaze_opts.hpp`).
    ///
    /// `locators` and `locatorDedupKeys` are parallel arrays of equal
    /// length (enforced via `AppendLocator` below): the former is
    /// the human-facing path (display tooltip + the path actually
    /// passed to `QFile::open`); the latter is the normalised form
    /// used for byte-equality dedup. On Windows the two diverge
    /// because the filesystem is case-insensitive but the user
    /// expects to see the case they typed (a recents tooltip
    /// reading "c:/users/jane/server.log" when the user typed
    /// "C:/Users/Jane/Server.log" was the pre-fix bug). On
    /// Linux / macOS the dedup key is identical to the display
    /// path; the two-vector shape costs one extra `std::string` per
    /// locator across all platforms, which is invisible against
    /// session JSON sizes already dominated by columns / filters.
    struct Source
    {
        enum class Kind
        {
            File,
            NetworkStream
        };
        Kind kind = Kind::File;
        /// Original-case absolute paths for `Kind::File` (one entry
        /// per appended file in load order); single-element for
        /// `Kind::NetworkStream` (producer URI / display name).
        /// Always parallel to `locatorDedupKeys` -- both vectors
        /// must be mutated together via `AppendLocator` /
        /// `ClearLocators`; direct `push_back` on either alone
        /// breaks the invariant.
        std::vector<std::string> locators;
        /// Canonicalised dedup keys (lower-cased + forward-slashed
        /// absolute paths on Windows; identical to `locators[i]`
        /// on case-sensitive platforms). The "are these two paths
        /// the same file?" check inside the open / append paths
        /// compares this rather than `locators[i]`, so a user
        /// dropping the same file with two different casings on
        /// Windows still gets a single entry.
        std::vector<std::string> locatorDedupKeys;
    };

    /// Required: drives the column layout for every consumer.
    std::vector<Column> columns;

    /// Session-only fields; default-valued when the file on disk is a
    /// columns-only configuration.
    std::vector<LogFilter> filters;
    Sort sort;
    std::optional<Source> source;
};

/// Case-insensitive match against known log-level field names (`level`,
/// `severity`, ...). `LogTable` uses this to gate `Enumeration -> Level`
/// promotion.
[[nodiscard]] bool IsLogLevelKey(const std::string &key);

/// Inline helper for the recurring `source.has_value() &&
/// !source->locators.empty()` shape -- the "source descriptor is
/// actionable" predicate. Centralising the gate prevents a half-checked
/// form (`has_value()` without `empty()`) from sneaking through one
/// call site at a time: a `Source{kind: File, locators: {}}` parses
/// but is not actionable, so every accessor that wants "do we have a
/// rebindable source" must check both. `noexcept` because both
/// operations on `std::optional` / `std::vector` are noexcept.
[[nodiscard]] inline bool HasLocators(const std::optional<LogConfiguration::Source> &source) noexcept
{
    return source.has_value() && !source->locators.empty();
}

/// Append a locator to @p target, keeping `locators` and
/// `locatorDedupKeys` in lockstep. Every production call site that
/// mutates `Source::locators` MUST go through here (or
/// `ClearLocators` below) so the parallel-array invariant cannot be
/// broken by a caller forgetting one half. The helper takes the
/// already-computed @p dedupKey rather than re-computing it from
/// @p displayPath because the canonicalisation lives in the
/// application layer (`logapp::CanonicalLocator`) and the library
/// deliberately has no Qt dependency. (Parameter is named `target`
/// rather than `source` to avoid shadowing the `LogConfiguration::source`
/// member at call sites that inline this helper.)
inline void AppendLocator(LogConfiguration::Source &target, std::string displayPath, std::string dedupKey)
{
    target.locators.push_back(std::move(displayPath));
    target.locatorDedupKeys.push_back(std::move(dedupKey));
}

/// Drop every locator on @p target, keeping `locators` and
/// `locatorDedupKeys` in lockstep. Trivially short, but preserving
/// the same chokepoint pattern as `AppendLocator` makes the
/// invariant impossible to forget when a future refactor needs to
/// wipe a source mid-flow.
inline void ClearLocators(LogConfiguration::Source &target)
{
    target.locators.clear();
    target.locatorDedupKeys.clear();
}

/// Selects which fields `Save` writes. Both shapes share one JSON
/// schema -- a `ColumnsOnly` file is a `Full` file with the
/// session-only members defaulted.
enum class SaveScope
{
    /// Writes only `columns`; portable across data sources.
    ColumnsOnly,
    /// Writes the full struct (columns + filters + sort + source).
    Full
};

/// Loads, saves, and updates a `LogConfiguration` from observed data.
class LogConfigurationManager
{
public:
    LogConfigurationManager() = default;

    /// Throws `std::runtime_error` on open failure.
    void Load(const std::filesystem::path &path);

    /// Parse a configuration directly from an in-memory buffer.
    /// Throws `std::runtime_error` on parse failure. Used by the
    /// app-side "could this file be a configuration?" probe so it
    /// can read a bounded prefix from disk rather than streaming
    /// the entire file through the IO path -- protects against
    /// pathological multi-gigabyte log files that happen to start
    /// with a `{"columns":[...]}` line.
    void LoadFromString(std::string_view content);
    /// Writes the full struct (equivalent to `SaveScope::Full`).
    void Save(const std::filesystem::path &path) const;
    /// Writes the subset selected by @p scope.
    void Save(const std::filesystem::path &path, SaveScope scope) const;

    /// Free-standing serialization for callers that already hold a
    /// `LogConfiguration` value (e.g. the app-side session-history
    /// manager auto-saving a snapshot). Throws on serialization or
    /// open failure -- same contract as the instance overload.
    /// No default for @p scope so callers are forced to opt in
    /// explicitly: a silent default tends to drift between
    /// "Columns only" (faster, drops filters / sort / source) and
    /// "Full" (recents-restorable) as new fields are added to the
    /// schema, and a snapshot that loses its source is invisible
    /// until restore time.
    static void Save(const LogConfiguration &configuration, const std::filesystem::path &path, SaveScope scope);

    /// Rebuilds the configuration from @p logData. Not safe mid-stream.
    void Update(const LogData &logData);

    /// Wipe the configuration back to a default-constructed
    /// `LogConfiguration` (no columns, no filters, no sort, no
    /// source). Invalidates the key cache. Used by `MainWindow::
    /// NewSession` to produce a true blank-window state -- the
    /// previous behavior preserved columns across NewSession so the
    /// user could reuse the layout, but that left stale headers /
    /// sort / source attached to an otherwise empty view, which
    /// did not match the user's "fresh window" mental model.
    void Reset();

    /// Replace the held `LogConfiguration` wholesale. Lets callers
    /// pre-parse a configuration from disk (e.g. `MainWindow`'s
    /// recents pre-flight) and apply it atomically without paying
    /// the second file read that `Load(path)` would. Invalidates
    /// the key cache so the next `Keys()` call rebuilds.
    void SetConfiguration(LogConfiguration configuration);

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

    /// Toggle `Column::autoDetect`. `true` hands the column to the
    /// auto-detector; `false` pins it at the current `type`. No-op
    /// out of range.
    void SetColumnAutoDetect(size_t columnIndex, bool autoDetect);

    /// Write `type` and `autoDetect` atomically. Editor path uses
    /// this so observers never see an intermediate `(newType,
    /// staleAutoDetect)` pair. No-op out of range.
    void SetColumnTypePair(size_t columnIndex, LogConfiguration::Type type, bool autoDetect);

    /// Toggle `Column::visible`. The column stays in the table; only
    /// the header section is hidden. No-op out of range.
    void SetColumnVisible(size_t columnIndex, bool visible);

    /// Replace `Column::header`. Display-only -- `keys` remains the
    /// stable identifier -- so renaming is safe at any point. No-op
    /// out of range.
    void SetColumnHeader(size_t columnIndex, std::string header);

    /// Replace `Column::printFormat`. Used by the editor when pinning
    /// a column to `Type::Time` so the cell formatting actually
    /// shows a date instead of the raw bytes. No-op out of range.
    void SetColumnPrintFormat(size_t columnIndex, std::string printFormat);

    /// Replace `Column::parseFormats`. Companion to
    /// `SetColumnPrintFormat`; without parse formats the Time
    /// back-fill walks the rows but matches nothing. No-op out of range.
    void SetColumnParseFormats(size_t columnIndex, std::vector<std::string> parseFormats);

    /// Replace `LogConfiguration::filters` wholesale. The app
    /// mirrors its runtime filter map through this before `Save` and
    /// before `MoveColumn`'s row remap.
    void SetFilters(std::vector<LogConfiguration::LogFilter> filters);

    /// Replace `LogConfiguration::sort`. Called by the session-state
    /// mirror before a `Full` save.
    void SetSort(LogConfiguration::Sort sort);

    /// Replace `LogConfiguration::source`. `nullopt` clears the binding.
    void SetSource(std::optional<LogConfiguration::Source> source);

    /// Apply `(srcIndex -> destIndex)` to a stored column index.
    /// Out-of-range inputs (including negative sentinels like
    /// `Sort::columnIndex == -1`) pass through unchanged. Exposed
    /// so the app can remap its runtime filter map with the same
    /// logic.
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
