#pragma once

#include "commands.hpp"
#include "diagnostics.hpp"

#include <loglib/filter_expression.hpp>
#include <loglib/log_table.hpp>

#include <expected>
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

/**
 * @brief Builds the combined filter used by `query` and `stats`.
 *
 * @param command Subcommand name for error lines (`query`, `stats`, …).
 */
[[nodiscard]] std::expected<loglib::FilterExpression, CommandOutput> BuildRowFilter(
    const Options &options, const loglib::LogTable &table, const RuntimeContext &runtime, std::string_view command
);

/** @brief True when any row-restricting flag besides `--allow-empty` is set. */
[[nodiscard]] bool HasActiveRowFilter(const Options &options) noexcept;

} // namespace slp
