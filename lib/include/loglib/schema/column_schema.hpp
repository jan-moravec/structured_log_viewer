/**
 * @brief Description of a table column as the user configured it.
 *
 * A Column names the log keys it shows, how its values are parsed and printed, and which type it has. The type decides
 * how the column is detected, sorted and filtered. The header also defines the default time formats. Everything here is
 * a plain value that can be saved to and loaded from JSON.
 *
 * This is part of the schema module.
 */
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace loglib
{

/**
 * @brief The type of a column. It decides how the column is detected, shown, sorted and filtered.
 *
 * Automatic detection looks at a column only if its type is Any and its autoDetect flag is set. In the saved JSON the
 * keys are written in lowerCamelCase.
 *
 * - Any: the starting type of a new column, and the type of columns the detector could not classify. With autoDetect
 *   off, it means the user wants the column treated as text. It sorts and filters as a string.
 * - String: a column the detector found to hold strings.
 * - Boolean: true and false, with false sorting first.
 * - Integer: only integers were seen.
 * - Floating: only floating-point numbers were seen.
 * - Number: a mix of integers and floating-point numbers.
 * - Time: timestamps.
 * - Enumeration: a small set of values, stored as ids into a dictionary.
 * - Level: an enumeration of log levels. It sorts, filters and is styled by severity.
 */
enum class ColumnType
{
    Any,
    String,
    Boolean,
    Integer,
    Floating,
    Number,
    Time,
    Enumeration,
    Level,
};

/**
 * @brief One column of the table, as saved in the configuration: its header, keys, type and options for display and
 * parsing.
 */
struct Column
{
    /** @brief The title shown above the column. */
    std::string header;

    /** @brief The log keys whose values fill the column. A row uses the first key it has. */
    std::vector<std::string> keys;

    /**
     * @brief How values are shown. For times it is a time format, and for numbers and booleans a fmt format string.
     * Strings ignore it.
     */
    std::string printFormat;

    /**
     * @brief The column type. New columns start as Any, which makes them candidates for detection.
     *
     * Turning a column into a Time column is permanent until the table is reset. A column detected as an enumeration
     * can be demoted when it overflows, but only while autoDetect is on.
     */
    ColumnType type = ColumnType::Any;

    /** @brief The time formats to try, in order, for a Time column. */
    std::vector<std::string> parseFormats;

    /**
     * @brief Whether a view shows the column. A hidden column stays in the table, so sorting and filtering still use
     * its data. A saved configuration without this field loads as visible.
     */
    bool visible = true;

    /**
     * @brief Extra level names for a Level column, as pairs of an alias and a level name.
     *
     * An alias matches the text of a value without regard to case, and the level name must be one of the LogLevel
     * names such as Info or Warn. These pairs are added to the built-in aliases. Other columns ignore them.
     */
    std::vector<std::pair<std::string, std::string>> levelMapping;

    /**
     * @brief True when the detector owns the column: it classifies Any columns and demotes enumerations that overflow.
     * False when the user fixed the type, and then it never changes by itself. A saved configuration without this
     * field loads as true.
     */
    bool autoDetect = true;
};

/**
 * @brief The time formats a new Time column starts with, also used when the user sets a column to Time and leaves the
 * list empty.
 *
 * The list is kept in one place so that every place that seeds it agrees. The order matters, because formats are tried
 * in order and the first match wins. The ISO 8601 forms come first because they cover most JSON, logfmt and RFC 5424
 * logs, and they take a fast path. The other formats match the shipped regex templates, so their timestamps become
 * times without the user editing the column:
 * - %d/%b/%Y:%H:%M:%S %z is the Apache and nginx common log format.
 * - %b %e %H:%M:%S and %b %d %H:%M:%S are the RFC 3164 syslog header, with a space or zero padded day and no year. The
 *   current year is filled in.
 * - %a %b %d %H:%M:%S %Y is the Apache error log. %S also reads an optional fraction of a second.
 *
 * Every added format widens the set of values treated as times. Keep a new format specific, with literal separators,
 * so it fails on other text. A format that is too loose at the end of the list can turn a numeric column into a Time
 * column.
 */
[[nodiscard]] inline std::vector<std::string> DefaultTimeParseFormats()
{
    return {
        "%FT%T%Ez",
        "%F %T%Ez",
        "%FT%T",
        "%F %T",
        "%d/%b/%Y:%H:%M:%S %z",
        "%b %e %H:%M:%S",
        "%b %d %H:%M:%S",
        "%a %b %d %H:%M:%S %Y",
    };
}

} // namespace loglib
