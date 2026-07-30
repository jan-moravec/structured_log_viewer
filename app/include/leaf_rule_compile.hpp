#pragma once

#include <loglib/filter_expression.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_filter.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace loglib
{
class LogTable;
} // namespace loglib

/// Return the index of the column whose `keys` are a superset of
/// @p keys, or -1 if none matches or @p keys is empty. Shared by
/// filter leaves and highlight rules so both bind identically.
[[nodiscard]] int ResolveLeafColumnByKeys(
    const std::vector<std::string> &keys, const std::vector<loglib::LogConfiguration::Column> &columns
) noexcept;

/// Compile @p rule to a `RowPredicate`, or `nullopt` when it is
/// inert (unresolved column, incomplete payload, or enum/level
/// rule against a column without a dictionary yet). The caller
/// (`CompileExpression`) treats inert leaves as match-all inside
/// `And` and match-none inside `Or`.
///
/// Level columns are expanded to every raw dictionary alias via
/// `LevelRankCache`, so a rule saved as `"Warn"` also matches rows
/// parsed from `WARN`, `warning`, or a user-defined alias.
[[nodiscard]] std::optional<loglib::RowPredicate> CompileLeaf(
    const loglib::LeafRule &rule,
    int resolvedColumn,
    const std::vector<loglib::LogConfiguration::Column> &columns,
    const loglib::LogTable *table
);

/// Field-by-field copy of a `HighlightRule` into a `LeafRule` so
/// both feed the same `CompileLeaf`.
[[nodiscard]] loglib::LeafRule ToLeafRule(const loglib::LogConfiguration::HighlightRule &rule);

/// Compile @p expression into a `CompiledFilterExpression`. Every
/// `And`/`Or` node's children are sorted cheap-first by
/// `EstimatedCost` so short-circuit evaluation fires the fastest
/// rejecting/accepting leaf first.
[[nodiscard]] loglib::CompiledFilterExpression CompileExpression(
    const loglib::FilterExpression &expression,
    const std::vector<loglib::LogConfiguration::Column> &columns,
    const loglib::LogTable *table
);
