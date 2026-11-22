/**
 * @brief Turns filter and highlight rules into row predicates.
 *
 * A condition names its column by log keys. These functions find the column for those keys, check a filter for
 * problems, and compile a condition or a whole filter expression against a table into a predicate that tests one row.
 * Filters and highlight rules use the same code, so both bind to columns the same way.
 *
 * This is part of the query module.
 */
#pragma once

#include "loglib/config/column_layout.hpp"
#include "loglib/config/filter_expression.hpp"
#include "loglib/config/log_configuration.hpp"
#include "loglib/query/row_filter.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace loglib
{

class LogTable;

/**
 * @brief Finds the first column whose keys include all of the given keys. Filters and highlight rules both use it, so
 * they find columns the same way.
 *
 * It returns the index of that column, or nullopt if no column matches and for an empty list of keys.
 */
[[nodiscard]] std::optional<size_t> FindColumnByKeys(
    const std::vector<std::string> &keys, const std::vector<Column> &columns
) noexcept;

/**
 * @brief Builds the row predicate for one condition, which finds its column in the table.
 *
 * Filters and highlight rules both compile their condition with this function, so they bind to columns and treat
 * incomplete rules the same way. It returns nullopt when the condition has no effect: its column was not found, the
 * condition is incomplete, or an enumeration or level condition refers to a column that has no dictionary yet.
 * CompileFilter treats such a condition as match-all inside an And and drops it from an Or.
 *
 * For a Level column the condition is expanded to all raw values that map to the level, taken from LevelRankCache. A
 * condition saved as Warn therefore also matches rows that say WARN, warning or a user-defined alias.
 *
 * The predicate holds the index of its column and the dictionary of the table, because that is what makes it fast to
 * run. Compile again after the table's columns move or change type, and after an enumeration dictionary is replaced.
 * The table must outlive the predicate's use.
 */
[[nodiscard]] std::optional<RowPredicate> CompileCondition(const FilterCondition &condition, const LogTable &table);

/**
 * @brief Lists the conditions of an expression that cannot work as written: columns that no column of the layout has,
 * and regular expressions that are invalid.
 *
 * CompileFilter accepts both, because a live source may still get the column later. A caller that has finished loading
 * should report these problems and not filter with them. The result has one message per problem and is empty when the
 * expression is sound.
 */
[[nodiscard]] std::vector<std::string> FindFilterProblems(
    const FilterExpression &expression, const ColumnLayout &layout
);

/**
 * @brief Builds a CompiledFilterExpression from an expression, with the columns and dictionaries of the table.
 *
 * The children of every And and Or are ordered by estimated cost, cheapest first, so that stopping early skips the
 * expensive tests. A missing root becomes match-all. Each condition is compiled with CompileCondition.
 *
 * The result holds column indices. Compile again after the table's columns move or change type. Its referencedColumns
 * tell a view whether a change to one column affects the filter, so the view can skip compiling when another column
 * changed.
 */
[[nodiscard]] CompiledFilterExpression CompileFilter(const FilterExpression &expression, const LogTable &table);

} // namespace loglib
