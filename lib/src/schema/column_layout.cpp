#include "loglib/schema/column_layout.hpp"
#include "loglib/core/ascii_case.hpp"
#include "loglib/schema/internal/level_column_slot.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <string_view>
#include <utility>

namespace
{

constexpr const char *DEFAULT_TIME_PRINT_FORMAT = "%F %H:%M:%S";

std::string ToLower(const std::string &str)
{
    std::string lower = str;
    std::ranges::transform(lower, lower.begin(), [](auto c) { return static_cast<unsigned char>(std::tolower(c)); });
    return lower;
}

// Common JSON and structured-log timestamp field names. The bare name t is left out because it gives false positives.
bool IsTimestampKey(const std::string &key)
{
    static const std::vector<std::string> KEYS = {"timestamp", "time", "ts", "@timestamp", "datetime", "created_at"};
    return std::ranges::any_of(KEYS, [lowerKey = ToLower(key)](const std::string &value) {
        return (lowerKey == value);
    });
}

/**
 * @brief True if fmt accepts the format for at least one of the value types a cell can hold. A format such as {:d} is
 * fine for integers and not for doubles, so only a format that fails for every type counts as invalid.
 */
bool PrintFormatIsUsable(const std::string &format)
{
    constexpr double SAMPLE_DOUBLE = 1.5;
    const auto accepts = [&format](const auto &sample) {
        try
        {
            static_cast<void>(fmt::vformat(format, fmt::make_format_args(sample)));
            return true;
        }
        catch (const fmt::format_error &)
        {
            return false;
        }
    };
    return accepts(std::int64_t{1}) || accepts(std::uint64_t{1}) || accepts(SAMPLE_DOUBLE) || accepts(true);
}

} // namespace

namespace loglib
{

bool IsLogLevelKey(const std::string &key)
{
    // The names are matched without regard to case and without allocating, because this runs once per column per
    // batch. The result is advisory: a false positive, such as a length column called l, is caught by the dictionary
    // check in LogTable.
    static constexpr std::array<std::string_view, 23> LEVEL_KEYS = {
        // Long-form and classic.
        "level",
        "severity",
        "loglevel",
        "log.level",
        "log_level",
        "lvl",
        "levelname",
        "priority",
        // Short forms of compact and embedded loggers.
        "l",
        "lv",
        "lev",
        "sev",
        "s",
        "loglvl",
        // OpenTelemetry, ECS and GCP.
        "severity_text",
        "severity.text",
        "severitytext",
        "log_severity",
        "log.severity",
        "logseverity",
        // Separator variants of levelname.
        "level_name",
        "level.name",
        // Serilog and Datadog.
        "@level",
    };
    const std::string_view keyView(key);
    return std::ranges::any_of(LEVEL_KEYS, [keyView](std::string_view value) {
        return EqualsIgnoreCaseAscii(keyView, value);
    });
}

ColumnLayout::ColumnLayout(std::vector<Column> columns)
    : mColumns(std::move(columns))
{
    SanitizePrintFormats();
}

const std::vector<Column> &ColumnLayout::Columns() const
{
    return mColumns;
}

void ColumnLayout::SetColumns(std::vector<Column> columns)
{
    mColumns = std::move(columns);
    mCacheStale = true;
    SanitizePrintFormats();
}

void ColumnLayout::Reset()
{
    mColumns.clear();
    mCacheStale = true;
}

std::vector<std::string> ColumnLayout::TakeWarnings()
{
    return std::exchange(mWarnings, {});
}

void ColumnLayout::SetTimeFormatCheck(TimeFormatCheck check)
{
    mTimeFormatCheck = std::move(check);
}

void ColumnLayout::SanitizePrintFormats()
{
    for (Column &column : mColumns)
    {
        if (column.printFormat.empty())
        {
            // No format was given, which is the default of a column. This is not an error and has no warning.
            column.printFormat = column.type == ColumnType::Time ? DEFAULT_TIME_PRINT_FORMAT : "{}";
            continue;
        }
        if (column.type == ColumnType::Time)
        {
            // A Time column holds a time format, not an fmt format.
            if (!mTimeFormatCheck || mTimeFormatCheck(column.printFormat))
            {
                continue;
            }
            mWarnings.push_back(
                fmt::format(
                    "Column '{}': the time format '{}' is not valid and was replaced by '{}'.",
                    column.header,
                    column.printFormat,
                    DEFAULT_TIME_PRINT_FORMAT
                )
            );
            column.printFormat = DEFAULT_TIME_PRINT_FORMAT;
            continue;
        }
        if (PrintFormatIsUsable(column.printFormat))
        {
            continue;
        }
        mWarnings.push_back(
            fmt::format(
                "Column '{}': the print format '{}' is not valid and was replaced by {{}}.",
                column.header,
                column.printFormat
            )
        );
        column.printFormat = "{}";
    }
}

void ColumnLayout::AppendKeys(const std::vector<std::string> &newKeys)
{
    // Columns are only appended, because the Qt column insertion that follows expects appends.
    EnsureKeyCacheBuilt();
    for (const std::string &key : newKeys)
    {
        if (IsKeyInAnyColumnCached(key))
        {
            continue;
        }
        if (IsTimestampKey(key))
        {
            mColumns.push_back(
                Column{
                    .header = key,
                    .keys = {key},
                    .printFormat = DEFAULT_TIME_PRINT_FORMAT,
                    .type = ColumnType::Time,
                    .parseFormats = DefaultTimeParseFormats(),
                    // The key name made this a Time column, not the detector, so a saved file keeps the type.
                    .autoDetect = false,
                }
            );
        }
        else
        {
            mColumns.push_back(
                Column{.header = key, .keys = {key}, .printFormat = "{}", .type = ColumnType::Any, .parseFormats = {}}
            );
        }
        mKeysInColumns.insert(key);
    }
}

bool ShouldBubbleLevelColumn(const ColumnLayout &layout, size_t columnIndex) noexcept
{
    const auto &columns = layout.Columns();
    if (columnIndex >= columns.size())
    {
        return false;
    }
    if (columns.size() < 2)
    {
        return false;
    }
    if (columnIndex == CANONICAL_LEVEL_COLUMN_INDEX)
    {
        return false;
    }
    // The first Level column keeps the canonical slot when a payload has more than one Level key, such as level and
    // severity. Without this guard, handling several of them in turn would swap the occupant of the slot each time.
    if (CANONICAL_LEVEL_COLUMN_INDEX < columns.size() &&
        columns[CANONICAL_LEVEL_COLUMN_INDEX].type == ColumnType::Level)
    {
        return false;
    }
    return true;
}

void ColumnLayout::MoveColumn(size_t srcIndex, size_t destIndex)
{
    if (srcIndex == destIndex || srcIndex >= mColumns.size() || destIndex >= mColumns.size())
    {
        return;
    }
    using Diff = std::vector<Column>::difference_type;
    const auto begin = mColumns.begin();
    if (srcIndex > destIndex)
    {
        std::rotate(
            std::next(begin, static_cast<Diff>(destIndex)),
            std::next(begin, static_cast<Diff>(srcIndex)),
            std::next(begin, static_cast<Diff>(srcIndex + 1))
        );
    }
    else
    {
        std::rotate(
            std::next(begin, static_cast<Diff>(srcIndex)),
            std::next(begin, static_cast<Diff>(srcIndex + 1)),
            std::next(begin, static_cast<Diff>(destIndex + 1))
        );
    }
    // Filters, highlight rules and the sort bind to columns by keys, so a reorder needs no fix-up. The key cache does
    // not change either.
}

void ColumnLayout::SetColumnType(size_t columnIndex, ColumnType type)
{
    if (columnIndex >= mColumns.size())
    {
        return;
    }
    mColumns[columnIndex].type = type;
}

void ColumnLayout::SetColumnAutoDetect(size_t columnIndex, bool autoDetect)
{
    if (columnIndex >= mColumns.size())
    {
        return;
    }
    mColumns[columnIndex].autoDetect = autoDetect;
}

void ColumnLayout::SetColumnTypePair(size_t columnIndex, ColumnType type, bool autoDetect)
{
    if (columnIndex >= mColumns.size())
    {
        return;
    }
    auto &column = mColumns[columnIndex];
    column.type = type;
    column.autoDetect = autoDetect;
}

void ColumnLayout::SetColumnVisible(size_t columnIndex, bool visible)
{
    if (columnIndex >= mColumns.size())
    {
        return;
    }
    mColumns[columnIndex].visible = visible;
}

void ColumnLayout::SetColumnHeader(size_t columnIndex, std::string header)
{
    if (columnIndex >= mColumns.size())
    {
        return;
    }
    mColumns[columnIndex].header = std::move(header);
}

void ColumnLayout::SetColumnPrintFormat(size_t columnIndex, std::string printFormat)
{
    if (columnIndex >= mColumns.size())
    {
        return;
    }
    mColumns[columnIndex].printFormat = std::move(printFormat);
    SanitizePrintFormats();
}

void ColumnLayout::SetColumnParseFormats(size_t columnIndex, std::vector<std::string> parseFormats)
{
    if (columnIndex >= mColumns.size())
    {
        return;
    }
    mColumns[columnIndex].parseFormats = std::move(parseFormats);
}

size_t ColumnLayout::CountAppendableKeys(const std::vector<std::string> &newKeys) const
{
    if (newKeys.empty())
    {
        return 0;
    }
    EnsureKeyCacheBuilt();
    // This mirrors the skip rule of AppendKeys and also counts a key repeated in the input once.
    std::unordered_set<std::string_view> alreadyCounted;
    alreadyCounted.reserve(newKeys.size());
    size_t count = 0;
    for (const std::string &key : newKeys)
    {
        if (IsKeyInAnyColumnCached(key))
        {
            continue;
        }
        if (alreadyCounted.insert(std::string_view(key)).second)
        {
            ++count;
        }
    }
    return count;
}

void ColumnLayout::EnsureKeyCacheBuilt() const
{
    if (!mCacheStale)
    {
        return;
    }
    mKeysInColumns.clear();
    mKeysInColumns.reserve(mColumns.size() * 4);
    for (const Column &column : mColumns)
    {
        for (const std::string &key : column.keys)
        {
            mKeysInColumns.insert(key);
        }
    }
    mCacheStale = false;
}

bool ColumnLayout::IsKeyInAnyColumnCached(const std::string &key) const
{
    EnsureKeyCacheBuilt();
    return mKeysInColumns.contains(key);
}

int FirstTimeColumnIndex(const ColumnLayout &layout)
{
    const auto &columns = layout.Columns();
    for (size_t i = 0; i < columns.size(); ++i)
    {
        if (columns[i].type == ColumnType::Time)
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::vector<Column> TimeColumnsOf(const ColumnLayout &layout)
{
    return TimeColumnsOf(std::span<const Column>(layout.Columns()));
}

std::vector<Column> TimeColumnsOf(std::span<const Column> columns)
{
    std::vector<Column> result;
    for (const Column &column : columns)
    {
        if (column.type == ColumnType::Time)
        {
            result.push_back(column);
        }
    }
    return result;
}

} // namespace loglib
