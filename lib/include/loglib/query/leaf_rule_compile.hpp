/**
 * @brief Turns filter and highlight rules into row predicates.
 *
 * A rule names its column by log keys. These functions find the column for those keys, check a rule for problems, and
 * compile a leaf or a whole filter expression into a predicate that tests one row. Filters and highlight rules use the
 * same code, so both bind to columns the same way.
 *
 * This is part of the query module.
 */
#pragma once

#include "loglib/query/log_filter.hpp"
#include "loglib/schema/filter_expression.hpp"
#include "loglib/schema/highlight_rule.hpp"
#include "loglib/schema/log_configuration.hpp"

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
 * It returns -1 if no column matches, and for an empty list of keys.
 */
[[nodiscard]] int ResolveLeafColumnByKeys(
    const std::vector<std::string> &keys, const std::vector<Column> &columns
) noexcept;

/**
 * @brief Builds the row predicate for one leaf rule.
 *
 * It returns nullopt when the leaf has no effect: the column was not found, the rule is incomplete, or an enumeration
 * or level rule refers to a column that has no dictionary yet. CompileExpression treats such a leaf as match-all inside
 * an And and drops it from an Or.
 *
 * For a Level column the rule is expanded to all raw values that map to the level, taken from LevelRankCache. A rule
 * saved as Warn therefore also matches rows that say WARN, warning or a user-defined alias.
 *
 * @param resolvedColumn The column found by ResolveLeafColumnByKeys, or -1.
 * @param columns The columns, used to expand level aliases.
 * @param table The table that owns the enumeration dictionaries. May be null.
 */
[[nodiscard]] std::optional<RowPredicate> CompileLeaf(
    const LeafRule &rule, int resolvedColumn, const std::vector<Column> &columns, const LogTable *table
);

/**
 * @brief Copies the matching part of a highlight rule into a LeafRule. The display settings stay behind, and
 * CompileLeaf then handles both kinds of rule.
 */
[[nodiscard]] LeafRule ToLeafRule(const HighlightRule &rule);

/**
 * @brief Lists the leaves of an expression that cannot work as written: columns that no column of the list has, and
 * regular expressions that are invalid.
 *
 * CompileExpression accepts both, because a live source may still get the column later. A caller that has finished
 * loading should report these problems and not filter with them. The result has one message per problem and is empty
 * when the expression is sound.
 */
[[nodiscard]] std::vector<std::string> FindFilterProblems(
    const FilterExpression &expression, const std::vector<Column> &columns
);

/**
 * @brief Builds a CompiledFilterExpression from an expression.
 *
 * The children of every And and Or are ordered by estimated cost, cheapest first, so that stopping early skips the
 * expensive tests. A missing root becomes match-all. The table is passed on to CompileLeaf and may be null.
 */
[[nodiscard]] CompiledFilterExpression CompileExpression(
    const FilterExpression &expression, const std::vector<Column> &columns, const LogTable *table
);

} // namespace loglib
