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

/// Resolve @p keys against @p columns using subset-match semantics
/// (every key in @p keys must appear in the column's `keys` vector).
/// Returns the column index or -1 when no column matches or @p keys
/// is empty. Shared by filter leaves and highlight rules so both
/// paths use identical binding.
[[nodiscard]] int ResolveLeafColumnByKeys(
    const std::vector<std::string> &keys, const std::vector<loglib::LogConfiguration::Column> &columns
) noexcept;

/// Compile @p rule against @p columns / @p table into a concrete
/// `loglib::RowPredicate`.
///
/// Returns `std::nullopt` when the rule is inert: unresolved
/// column, incomplete payload, or an enum / level rule whose
/// column has no dictionary yet. The caller decides how to treat
/// inert leaves per-node (AND treats them as "match all", OR as
/// "match none"; the parent `CompileExpression` handles both).
///
/// Level columns are expanded to every raw dictionary alias via
/// `LevelRankCache`, so a rule saved as `"Warn"` matches rows
/// parsed from `WARN`, `warning`, and any user-defined
/// `levelMapping` alias. Previously duplicated between
/// `MainWindow::UpdateFilters` and `HighlightRuleSet::CompileRule`;
/// this is the shared owner.
[[nodiscard]] std::optional<loglib::RowPredicate> CompileLeaf(
    const loglib::LeafRule &rule,
    int resolvedColumn,
    const std::vector<loglib::LogConfiguration::Column> &columns,
    const loglib::LogTable *table
);

/// Adapt @p rule (a `HighlightRule`, which shares the enums and
/// payload layout with `LeafRule`) into a `LeafRule` for
/// `CompileLeaf`. Cheap: field-by-field copy.
[[nodiscard]] loglib::LeafRule ToLeafRule(const loglib::LogConfiguration::HighlightRule &rule);

/// Compile @p expression against @p columns and @p table into a
/// `CompiledFilterExpression`. Leaves whose column keys don't
/// resolve become inert (dropped in AND, "match none" in OR).
/// Cost ordering: children in every `And` / `Or` node are sorted
/// by `EstimatedCost` ascending, so short-circuit evaluation
/// fires the cheapest rejecting / accepting leaf first.
[[nodiscard]] loglib::CompiledFilterExpression CompileExpression(
    const loglib::FilterExpression &expression,
    const std::vector<loglib::LogConfiguration::Column> &columns,
    const loglib::LogTable *table
);
