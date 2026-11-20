/**
 * @brief The columns of a table and the operations that change them.
 *
 * ColumnLayout holds the ordered list of Column values that a table shows. It adds columns for new keys, moves, renames
 * and retypes them, and keeps their print formats valid. A table owns one layout and nothing else of a configuration:
 * the filter, sort, anchors and highlight rules belong to the session that shows the table and find their columns by
 * keys, so editing the layout never has to touch them.
 *
 * This is part of the schema module. LogConfiguration saves the columns of a layout to a file.
 */
#pragma once

#include "loglib/schema/column_schema.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

namespace loglib
{

/**
 * @brief True if the key is a known name for a log level field, such as level or severity, ignoring case. LogTable
 * only turns an enumeration into a Level column for such keys.
 */
[[nodiscard]] bool IsLogLevelKey(const std::string &key);

/**
 * @brief Where a Level column belongs: index 1, after the time column at index 0, so rows read as time, severity,
 * message.
 */
inline constexpr size_t CANONICAL_LEVEL_COLUMN_INDEX = 1;

/** @brief The ordered columns of a table, with the operations that edit them. */
class ColumnLayout
{
public:
    ColumnLayout() = default;

    /**
     * @brief Starts with the given columns. Like SetColumns, it checks the print formats and reports a replaced one
     * through TakeWarnings.
     */
    explicit ColumnLayout(std::vector<Column> columns);

    /** @brief The columns, in display order. */
    [[nodiscard]] const std::vector<Column> &Columns() const;

    /**
     * @brief Replaces all columns in one step.
     *
     * A print format that no value type accepts, such as {:q}, is replaced by {} and reported once through
     * TakeWarnings. Time columns are checked by the time format check.
     */
    void SetColumns(std::vector<Column> columns);

    /** @brief Removes every column, as for a new session. */
    void Reset();

    /** @brief Returns the messages about print formats replaced since the last call, and clears them. */
    [[nodiscard]] std::vector<std::string> TakeWarnings();

    /** @brief Tells whether a Time column can print with a time format. */
    using TimeFormatCheck = std::function<bool(const std::string &format)>;

    /**
     * @brief Sets the check that validates the print formats of Time columns.
     *
     * The schema module cannot read time formats itself, so the table that owns the layout supplies the check. With a
     * check set, a Time column whose format fails it gets the default time format, and the replacement is reported once
     * through TakeWarnings, like any other replaced print format. Without a check, Time formats are left alone.
     */
    void SetTimeFormatCheck(TimeFormatCheck check);

    /**
     * @brief Adds columns for keys that no column uses yet, and makes keys that look like timestamps Time columns.
     * Existing column indices do not change.
     */
    void AppendKeys(const std::vector<std::string> &newKeys);

    /** @brief How many columns AppendKeys would add for these keys. Changes nothing. */
    size_t CountAppendableKeys(const std::vector<std::string> &newKeys) const;

    /**
     * @brief Moves a column from srcIndex to destIndex. Filters, highlight rules and the sort find columns by their
     * keys, so they follow the column.
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

private:
    /**
     * @brief Replaces print formats that no value type accepts with {} and records a warning for each. Time columns are
     * checked with the time format check, and get the default time format.
     */
    void SanitizePrintFormats();

    void EnsureKeyCacheBuilt() const;
    bool IsKeyInAnyColumnCached(const std::string &key) const;

    std::vector<Column> mColumns;

    TimeFormatCheck mTimeFormatCheck;

    /** @brief Messages about print formats that were replaced. TakeWarnings hands them out once. */
    std::vector<std::string> mWarnings;

    /** @brief Every key that any column uses. It is rebuilt when mCacheStale is set, which the setters do. */
    mutable std::unordered_set<std::string> mKeysInColumns;
    mutable bool mCacheStale = true;
};

/**
 * @brief The index of the first Time column, or nullopt. That column is the one used as the timestamp of a row, for
 * example for summaries and time filters.
 */
[[nodiscard]] std::optional<size_t> FirstTimeColumnIndex(const ColumnLayout &layout);

/**
 * @brief The columns of type Time, in column order. A parser gets these as ParserOptions::timeColumns and converts them
 * on its worker threads, which is faster than the table converting them. A caller that passes none still gets correct
 * rows, because the table converts the columns the batch does not list as converted.
 */
[[nodiscard]] std::vector<Column> TimeColumnsOf(std::span<const Column> columns);

/** @brief The columns of type Time of a layout, in column order. */
[[nodiscard]] std::vector<Column> TimeColumnsOf(const ColumnLayout &layout);

} // namespace loglib
