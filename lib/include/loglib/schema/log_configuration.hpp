/**
 * @brief The saveable configuration of a log view and the manager that edits it.
 *
 * LogConfiguration collects the columns, the source description, the session view, anchors and highlight rules into one
 * value that can be written to JSON. LogConfigurationManager builds a configuration from the keys found in a log, keeps
 * it consistent when columns move, and loads and saves it.
 *
 * This is part of the schema module.
 */
#pragma once

#include "loglib/schema/anchor_entry.hpp"
#include "loglib/schema/column_schema.hpp"
#include "loglib/schema/filter_expression.hpp"
#include "loglib/schema/highlight_rule.hpp"
#include "loglib/schema/session_view.hpp"
#include "loglib/schema/source_descriptor.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace loglib
{

/**
 * @brief Everything that is saved about how a log is shown: columns, filter, sort, source, anchors and highlight rules.
 *
 * Each group is a type of its own: Column and ColumnType, Source and SourceLocator, Sort with the filter expression,
 * AnchorEntry and HighlightRule. The nested aliases let code write LogConfiguration::Column. In the saved JSON all
 * members are on one level.
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

    /** @brief The columns, in display order. A saved file must have them, and they decide the layout everywhere. */
    std::vector<Column> columns;

    /**
     * @brief The filter. The members up to anchors belong to one session and keep their defaults when a file holds
     * columns only.
     *
     * The default is an empty And, which accepts every row, so a new session shows everything. Leaves find their
     * column by LeafRule::columnKeys and not by position, so moving a column does not change the filter.
     */
    FilterExpression expression;

    /** @brief The saved sort. */
    Sort sort;

    /** @brief Where the log came from. Empty when the configuration is not tied to a source. */
    std::optional<Source> source;

    /**
     * @brief The anchors. They are written sorted by locator and line id so saved files differ little, and may be in
     * any order when read.
     */
    std::vector<AnchorEntry> anchors;

    /**
     * @brief The highlight rules. They are saved in both save scopes, so they follow the columns. Order matters,
     * because the last matching rule wins.
     */
    std::vector<HighlightRule> highlightRules;
};

/**
 * @brief True if the key is a known name for a log level field, such as level or severity, ignoring case. LogTable
 * only turns an enumeration into a Level column for such keys.
 */
[[nodiscard]] bool IsLogLevelKey(const std::string &key);

/**
 * @brief The index of the first Time column, or -1. That column is the one used as the timestamp of a row, for example
 * for summaries and time filters.
 */
[[nodiscard]] int FirstTimeColumnIndex(const LogConfiguration &configuration);

/**
 * @brief Chooses what Save writes. Both use one file format, and a ColumnsOnly file is a Full file with the session
 * members left at their defaults.
 */
enum class SaveScope
{
    /** @brief Columns and highlight rules only, so the file can be used with other logs. */
    ColumnsOnly,
    /** @brief Everything: columns, filter, sort, source, anchors and highlight rules. */
    Full
};

/** @brief Holds a LogConfiguration and loads, saves and updates it. */
class LogConfigurationManager
{
public:
    LogConfigurationManager() = default;

    /** @brief Loads a configuration file. Throws std::runtime_error if it cannot be opened or parsed. */
    void Load(const std::filesystem::path &path);

    /**
     * @brief Loads a configuration from text. Throws if it cannot be parsed. It lets a caller read a short prefix of a
     * file without going through a large log.
     */
    void LoadFromString(std::string_view content);

    /** @brief Saves everything, the same as SaveScope::Full. */
    void Save(const std::filesystem::path &path) const;

    /** @brief Saves the parts that scope selects. */
    void Save(const std::filesystem::path &path, SaveScope scope) const;

    /**
     * @brief Saves a configuration value that is not held by a manager. Throws if it cannot be written.
     *
     * The scope has no default, so a caller must choose between columns only and everything, and cannot be caught out
     * when the configuration grows.
     */
    static void Save(const LogConfiguration &configuration, const std::filesystem::path &path, SaveScope scope);

    /**
     * @brief Rebuilds the configuration from the sorted keys of the loaded data. Do not call it while a stream is
     * being parsed.
     */
    void Update(std::span<const std::string> sortedKeys);

    /** @brief Resets to an empty configuration, as for a new session. */
    void Reset();

    /** @brief Replaces the whole configuration with an already parsed one, in one step and without reading a file. */
    void SetConfiguration(LogConfiguration configuration);

    /**
     * @brief Adds columns for keys that no column uses yet, and makes keys that look like timestamps Time columns.
     * Existing column indices do not change.
     */
    void AppendKeys(const std::vector<std::string> &newKeys);

    /** @brief How many columns AppendKeys would add for these keys. Changes nothing. */
    size_t CountAppendableKeys(const std::vector<std::string> &newKeys) const;

    /**
     * @brief Moves a column from srcIndex to destIndex.
     *
     * Filters and highlight rules find columns by their keys, so they follow the column. The saved sort stores a column
     * index, and it is updated to follow the column too.
     */
    void MoveColumn(size_t srcIndex, size_t destIndex);

    /**
     * @brief Changes the type of a column. The caller must convert the existing rows. Like the other column setters,
     * it does nothing for an index out of range.
     */
    void SetColumnType(size_t columnIndex, ColumnType type);

    /** @brief Hands the column to the auto-detector with true, or fixes it at its current type with false. */
    void SetColumnAutoDetect(size_t columnIndex, bool autoDetect);

    /** @brief Sets the type and the auto-detect flag together, so no one sees a new type with the old flag. */
    void SetColumnTypePair(size_t columnIndex, ColumnType type, bool autoDetect);

    /** @brief Shows or hides the column in a view. The column stays in the table. */
    void SetColumnVisible(size_t columnIndex, bool visible);

    /** @brief Renames the column title. The keys identify the column, so renaming is safe at any time. */
    void SetColumnHeader(size_t columnIndex, std::string header);

    /**
     * @brief Sets how values are shown. A column fixed as a Time column needs a time format here, or its cells show
     * the raw text.
     */
    void SetColumnPrintFormat(size_t columnIndex, std::string printFormat);

    /**
     * @brief Sets the time formats to try. Without them, converting a Time column goes through the rows and converts
     * nothing.
     */
    void SetColumnParseFormats(size_t columnIndex, std::vector<std::string> parseFormats);

    /**
     * @brief Replaces the filter. Call it before Save and after the user edits the filter, so the saved file matches
     * what is shown.
     */
    void SetExpression(FilterExpression expression);

    /** @brief Replaces the saved sort. Call it before a full Save. */
    void SetSort(Sort sort);

    /** @brief Replaces the source. An empty value removes it. */
    void SetSource(std::optional<Source> source);

    /** @brief Replaces all anchors. An empty list removes them. */
    void SetAnchors(std::vector<AnchorEntry> anchors);

    /**
     * @brief Replaces all highlight rules. Rules find their column by keys, so nothing else needs fixing after a move.
     */
    void SetHighlightRules(std::vector<HighlightRule> rules);

    /**
     * @brief Returns where a stored column index ends up after a column moves from srcIndex to destIndex.
     *
     * An index out of range, including the -1 that Sort uses for no sort, is returned unchanged. Applications use it to
     * keep their own column indices in step.
     */
    [[nodiscard]] static int RemapColumnIndexAfterMove(int columnIndex, int srcIndex, int destIndex);

    const LogConfiguration &Configuration() const;

private:
    void EnsureKeyCacheBuilt() const;
    bool IsKeyInAnyColumnCached(const std::string &key) const;

    LogConfiguration mConfiguration;

    /** @brief Every key that any column uses. It is rebuilt when mCacheStale is set, which the setters do. */
    mutable std::unordered_set<std::string> mKeysInColumns;
    mutable bool mCacheStale = true;
};

/**
 * @brief Where a Level column belongs: index 1, after the time column at index 0, so rows read as time, severity,
 * message.
 */
inline constexpr size_t CANONICAL_LEVEL_COLUMN_INDEX = 1;

/**
 * @brief True if the column should move to CANONICAL_LEVEL_COLUMN_INDEX. False if the index is out of range, the
 * column is already there, there is only one column, or another Level column holds that place.
 */
[[nodiscard]] bool ShouldBubbleLevelColumn(const LogConfiguration &config, size_t columnIndex) noexcept;

} // namespace loglib
