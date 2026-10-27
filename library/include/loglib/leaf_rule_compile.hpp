#pragma once

#include "loglib/filter_expression.hpp"
#include "loglib/highlight_rule.hpp"
#include "loglib/log_configuration.hpp"
#include "loglib/log_filter.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace loglib
{

class LogTable;

/**
 * @brief Returns the first column whose keys are a superset of @p keys.
 *
 * Shared by filter leaves and highlight rules so both bind identically.
 * An empty @p keys list matches nothing.
 *
 * @param keys Rule column keys. Empty yields `-1`.
 * @param columns Columns to search, in display order.
 * @return Column index, or `-1` when no column matches.
 */
[[nodiscard]] int ResolveLeafColumnByKeys(
    const std::vector<std::string> &keys, const std::vector<Column> &columns
) noexcept;

/**
 * @brief Compiles @p rule to a row predicate.
 *
 * Returns `nullopt` when the leaf is inert: unresolved column,
 * incomplete payload, or an enum/level rule against a column that
 * has no dictionary yet. `CompileExpression` treats an inert leaf as
 * match-all inside `And` and drops it inside `Or`.
 *
 * Level columns expand to every raw dictionary alias via
 * `LevelRankCache`, so a rule saved as `"Warn"` also matches rows
 * parsed from `WARN`, `warning`, or a user-defined alias.
 *
 * @param rule Leaf to compile.
 * @param resolvedColumn Column index from `ResolveLeafColumnByKeys`, or `-1`.
 * @param columns Column list used for level-alias expansion.
 * @param table Table that owns enum dictionaries, or `nullptr`.
 * @return Predicate, or `nullopt` when the leaf carries no constraint.
 */
[[nodiscard]] std::optional<RowPredicate> CompileLeaf(
    const LeafRule &rule, int resolvedColumn, const std::vector<Column> &columns, const LogTable *table
);

/**
 * @brief Copies the filter payload of @p rule into a `LeafRule`.
 *
 * Rendering fields stay on `HighlightRule`. Both kinds then share
 * `CompileLeaf`.
 *
 * @param rule Highlight rule to project.
 * @return Leaf with the same match payload.
 */
[[nodiscard]] LeafRule ToLeafRule(const HighlightRule &rule);

/**
 * @brief Compiles @p expression into a `CompiledFilterExpression`.
 *
 * Every `And` / `Or` node's children are sorted cheap-first by
 * `EstimatedCost` so short-circuit evaluation tries the fastest
 * rejecting or accepting leaf first.
 *
 * @param expression Expression to compile.
 * @param columns Columns used to resolve leaf keys.
 * @param table Table passed through to `CompileLeaf`, or `nullptr`.
 * @return Compiled tree. An absent root becomes match-all.
 */
[[nodiscard]] CompiledFilterExpression CompileExpression(
    const FilterExpression &expression, const std::vector<Column> &columns, const LogTable *table
);

} // namespace loglib
