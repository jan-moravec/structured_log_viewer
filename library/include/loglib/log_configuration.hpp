#pragma once

#include "loglib/anchor_entry.hpp"
#include "loglib/column_schema.hpp"
#include "loglib/filter_expression.hpp"
#include "loglib/highlight_rule.hpp"
#include "loglib/session_view.hpp"
#include "loglib/source_descriptor.hpp"

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

/**
 * @brief Serializable configuration root.
 *
 * Value groups are named types with one responsibility each:
 * `Column` / `ColumnType` (schema), `Source` / `SourceLocator`
 * (persisted source), `SessionView` (`expression` + `Sort`),
 * `AnchorEntry`, and `HighlightRule`. Nested aliases keep existing
 * `LogConfiguration::Column` spellings compiling. JSON keys stay
 * flat on this root.
 */
struct LogConfiguration
{
    using Type = ColumnType;
    using Column = loglib::Column;
    using Sort = loglib::Sort;
    using SourceLocator = loglib::SourceLocator;
    using Source = loglib::Source;
    using AnchorEntry = loglib::AnchorEntry;
    using HighlightRule = loglib::HighlightRule;

    /** @brief Required: drives the column layout for every consumer. */
    std::vector<Column> columns;

    /**
     * @brief Session-only fields; default-valued when the file on disk is a
     * columns-only configuration.
     *
     * `expression` is the single boolean tree that drives the
     * filter view. Default-constructed `FilterExpression` is an
     * empty `And` node -- i.e. "match every row" -- so an unfilled
     * session is a plain view. Leaves bind columns by
     * `LeafRule::columnKeys` (durable across `MoveColumn` and
     * cross-source apply), which is why `MoveColumn` no longer
     * remaps filter row indices.
     */
    FilterExpression expression;
    Sort sort;
    std::optional<Source> source;

    /**
     * @brief Persisted anchors. Sorted by `(locator, lineId)` on save
     * for stable diffs; no ordering guarantee on read.
     */
    std::vector<AnchorEntry> anchors;

    /**
     * @brief Configuration-scope: written in both `SaveScope::Full`
     * and `SaveScope::ColumnsOnly` so highlight rules roam with
     * the columns. Order matters (last-match-wins per row).
     */
    std::vector<HighlightRule> highlightRules;
};

/**
 * @brief Case-insensitive match against known log-level field names (`level`,
 * `severity`, ...). `LogTable` uses this to gate `Enumeration -> Level`
 * promotion.
 */
[[nodiscard]] bool IsLogLevelKey(const std::string &key);

/**
 * @brief Index of the first `Type::Time` column in @p configuration, or -1.
 * Single source of truth for "which column is the canonical timestamp"
 * (Record Details summary, row right-click time-filter menu, ...).
 */
[[nodiscard]] int FirstTimeColumnIndex(const LogConfiguration &configuration);

/**
 * @brief Selects which fields `Save` writes. Both shapes share one JSON
 * schema -- a `ColumnsOnly` file is a `Full` file with the
 * session-only members defaulted.
 */
enum class SaveScope
{
    /** @brief Writes only `columns`; portable across data sources. */
    ColumnsOnly,
    /** @brief Writes the full struct (columns + filters + sort + source). */
    Full
};

/** @brief Loads, saves, and updates a `LogConfiguration` from observed data. */
class LogConfigurationManager
{
public:
    LogConfigurationManager() = default;

    /** @brief Throws `std::runtime_error` on open failure. */
    void Load(const std::filesystem::path &path);

    /**
     * @brief Parse from an in-memory buffer. Throws on parse failure.
     * Used by the app-side configuration probe so a bounded
     * prefix can be read from disk without streaming a
     * multi-gigabyte log through the IO path.
     */
    void LoadFromString(std::string_view content);
    /** @brief Writes the full struct (equivalent to `SaveScope::Full`). */
    void Save(const std::filesystem::path &path) const;
    /** @brief Writes the subset selected by @p scope. */
    void Save(const std::filesystem::path &path, SaveScope scope) const;

    /**
     * @brief Free-standing save for callers that already hold a
     * `LogConfiguration` value. Throws on serialization / open
     * failure. @p scope has no default so callers can't silently
     * drift between Columns-only and Full as the schema grows.
     */
    static void Save(const LogConfiguration &configuration, const std::filesystem::path &path, SaveScope scope);

    /** @brief Rebuilds the configuration from @p logData. Not safe mid-stream. */
    void Update(const LogData &logData);

    /**
     * @brief Wipe to a default-constructed `LogConfiguration`. Invalidates
     * the key cache. Used by `MainWindow::NewSession` to produce a
     * true blank-window state.
     */
    void Reset();

    /**
     * @brief Replace the held configuration wholesale. Lets callers
     * apply an already-parsed value atomically (no second file
     * read). Invalidates the key cache.
     */
    void SetConfiguration(LogConfiguration configuration);

    /**
     * @brief Append-only: adds keys not already configured, auto-promoting
     * timestamp-named ones. Existing column indices stay put.
     */
    void AppendKeys(const std::vector<std::string> &newKeys);

    /** @brief Non-mutating count of fresh columns `AppendKeys(newKeys)` would add. */
    size_t CountAppendableKeys(const std::vector<std::string> &newKeys) const;

    /**
     * @brief Move the column at @p srcIndex to @p destIndex.
     * `LogConfiguration::expression` binds leaves by column keys,
     * so no filter remap is required here -- rules follow their
     * column across reorders automatically. `sort.columnIndex`
     * still tracks the moved column and is remapped in step.
     */
    void MoveColumn(size_t srcIndex, size_t destIndex);

    /**
     * @brief Flip the type of the column at @p columnIndex; caller
     * back-fills row data. No-op out of range.
     */
    void SetColumnType(size_t columnIndex, ColumnType type);

    /**
     * @brief Toggle `Column::autoDetect`. `true` hands the column to the
     * auto-detector; `false` pins it at the current `type`. No-op
     * out of range.
     */
    void SetColumnAutoDetect(size_t columnIndex, bool autoDetect);

    /**
     * @brief Write `type` and `autoDetect` atomically. Editor path uses
     * this so observers never see an intermediate `(newType,
     * staleAutoDetect)` pair. No-op out of range.
     */
    void SetColumnTypePair(size_t columnIndex, ColumnType type, bool autoDetect);

    /**
     * @brief Toggle `Column::visible`. The column stays in the table; only
     * the header section is hidden. No-op out of range.
     */
    void SetColumnVisible(size_t columnIndex, bool visible);

    /**
     * @brief Replace `Column::header`. Display-only -- `keys` remains the
     * stable identifier -- so renaming is safe at any point. No-op
     * out of range.
     */
    void SetColumnHeader(size_t columnIndex, std::string header);

    /**
     * @brief Replace `Column::printFormat`. Used by the editor when pinning
     * a column to `Type::Time` so the cell formatting actually
     * shows a date instead of the raw bytes. No-op out of range.
     */
    void SetColumnPrintFormat(size_t columnIndex, std::string printFormat);

    /**
     * @brief Replace `Column::parseFormats`. Companion to
     * `SetColumnPrintFormat`; without parse formats the Time
     * back-fill walks the rows but matches nothing. No-op out of range.
     */
    void SetColumnParseFormats(size_t columnIndex, std::vector<std::string> parseFormats);

    /**
     * @brief Replace `LogConfiguration::expression` wholesale. The app
     * mirrors its runtime expression tree through this before
     * `Save` and after every user edit in the simple / Advanced
     * filter editors.
     */
    void SetExpression(FilterExpression expression);

    /**
     * @brief Replace `LogConfiguration::sort`. Called by the session-state
     * mirror before a `Full` save.
     */
    void SetSort(Sort sort);

    /** @brief Replace `LogConfiguration::source`. `nullopt` clears the binding. */
    void SetSource(std::optional<Source> source);

    /** @brief Replace `LogConfiguration::anchors`. Empty clears them all. */
    void SetAnchors(std::vector<AnchorEntry> anchors);

    /**
     * @brief Replace `LogConfiguration::highlightRules` wholesale. Rules
     * bind by column keys, so no `MoveColumn` remap is needed
     * afterwards.
     */
    void SetHighlightRules(std::vector<HighlightRule> rules);

    /**
     * @brief Apply `(srcIndex -> destIndex)` to a stored column index.
     * Out-of-range inputs (including negative sentinels like
     * `Sort::columnIndex == -1`) pass through unchanged. Exposed
     * so the app can remap its runtime filter map with the same
     * logic.
     */
    [[nodiscard]] static int RemapColumnIndexAfterMove(int columnIndex, int srcIndex, int destIndex);

    const LogConfiguration &Configuration() const;

private:
    void EnsureKeyCacheBuilt() const;
    bool IsKeyInAnyColumnCached(const std::string &key) const;

    LogConfiguration mConfiguration;

    /**
     * @brief Cached "every key referenced by any column"; mutators flip
     * `mCacheStale`.
     */
    mutable std::unordered_set<std::string> mKeysInColumns;
    mutable bool mCacheStale = true;
};

/**
 * @brief Canonical position for a `Type::Level` column (index 1, after
 * the time column at index 0). Reading order: time -> severity
 * -> message.
 */
inline constexpr size_t CANONICAL_LEVEL_COLUMN_INDEX = 1;

/**
 * @brief True iff a column at @p columnIndex should be bubbled to
 * `CANONICAL_LEVEL_COLUMN_INDEX`. False for out-of-range,
 * already-in-place, single-column, and "another Level column
 * already holds the slot" cases.
 */
[[nodiscard]] bool ShouldBubbleLevelColumn(const LogConfiguration &config, size_t columnIndex) noexcept;

/**
 * @brief Move @p columnIndex to `CANONICAL_LEVEL_COLUMN_INDEX` via
 * `mgr.MoveColumn` (so persisted `filters[*].row` is remapped).
 * No-op when `ShouldBubbleLevelColumn` returns false.
 *
 * Used by callers that only need to move the configuration
 * (notably unit tests). `LogTable::MaybePromoteToLevel` does its
 * own move via `LogTable::MoveColumn` to keep its parallel
 * `mColumnKeyIds` vector in sync.
 */
void BubbleLevelColumnToCanonicalPosition(LogConfigurationManager &mgr, size_t columnIndex);

} // namespace loglib
