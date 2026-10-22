#pragma once

#include "commands.hpp"
#include "diagnostics.hpp"

#include <loglib/filter_expression.hpp>
#include <loglib/log_table.hpp>
#include <loglib/log_value.hpp>

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace loglib
{
struct Column;
}

namespace slp
{

/** @brief Lists column headers for diagnostics. */
[[nodiscard]] std::string AvailableColumnList(const std::vector<loglib::Column> &columns);

/** @brief Resolves a column by header, key, or case-insensitive match. */
[[nodiscard]] int FindColumnByName(const std::vector<loglib::Column> &columns, std::string_view name);

/** @brief Index of the first column with @p type, or -1. */
[[nodiscard]] int FirstColumnOfType(const std::vector<loglib::Column> &columns, loglib::ColumnType type);

/** @brief Whether @p value holds anything. */
[[nodiscard]] bool IsPresent(const loglib::LogValue &value) noexcept;

/**
 * @brief Checks `--filter` syntax without a table.
 *
 * @return A usage failure with a caret diagnostic for the first malformed query.
 */
[[nodiscard]] std::optional<Failure> CheckFilterSyntax(const FilterOptions &filter);

/**
 * @brief Builds the combined row filter from the row flags.
 *
 * @param filter Row flags.
 * @param table Loaded table that supplies the time and level columns.
 * @param runtime Process context for time zone resolution.
 * @param command Subcommand name for error lines.
 */
[[nodiscard]] std::expected<loglib::FilterExpression, Failure> BuildRowFilter(
    const FilterOptions &filter, const loglib::LogTable &table, const RuntimeContext &runtime, std::string_view command
);

} // namespace slp
