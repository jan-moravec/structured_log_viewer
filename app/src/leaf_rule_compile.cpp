#include "leaf_rule_compile.hpp"

#include "log_string_matcher.hpp"

#include <loglib/enum_dictionary.hpp>
#include <loglib/filter_expression.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_filter.hpp>
#include <loglib/log_level.hpp>
#include <loglib/log_table.hpp>

#include <QString>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

int ResolveLeafColumnByKeys(
    const std::vector<std::string> &keys, const std::vector<loglib::LogConfiguration::Column> &columns
) noexcept
{
    if (keys.empty())
    {
        return -1;
    }
    // Subset match: every rule key must appear in the column's keys.
    for (std::size_t i = 0; i < columns.size(); ++i)
    {
        const auto &columnKeys = columns[i].keys;
        const bool allPresent = std::ranges::all_of(keys, [&columnKeys](const std::string &k) {
            return std::ranges::find(columnKeys, k) != columnKeys.end();
        });
        if (allPresent)
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

loglib::LeafRule ToLeafRule(const loglib::LogConfiguration::HighlightRule &rule)
{
    // Plain copy of shared payload; rendering fields (name,
    // enabled, colours, bold/italic) stay on `HighlightRule`.
    loglib::LeafRule leaf;
    leaf.type = rule.type;
    leaf.columnKeys = rule.columnKeys;
    leaf.matchType = rule.matchType;
    leaf.filterString = rule.filterString;
    leaf.filterBegin = rule.filterBegin;
    leaf.filterEnd = rule.filterEnd;
    leaf.filterMinValue = rule.filterMinValue;
    leaf.filterMaxValue = rule.filterMaxValue;
    leaf.filterValues = rule.filterValues;
    return leaf;
}

std::optional<loglib::RowPredicate> CompileLeaf(
    const loglib::LeafRule &rule,
    int resolvedColumn,
    const std::vector<loglib::LogConfiguration::Column> &columns,
    const loglib::LogTable *table
)
{
    if (resolvedColumn < 0)
    {
        return std::nullopt;
    }
    const auto column = static_cast<std::size_t>(resolvedColumn);
    using T = loglib::LeafRule::Type;
    switch (rule.type)
    {
    case T::Time:
    {
        // At least one bound required; INT64 sentinels fill the
        // open side so the visitor stays a simple `>=` / `<=` pair.
        if (!rule.filterBegin.has_value() && !rule.filterEnd.has_value())
        {
            return std::nullopt;
        }
        return loglib::RowPredicate{
            std::in_place_type<loglib::TimeRangeRowPredicate>,
            column,
            rule.filterBegin.value_or(std::numeric_limits<std::int64_t>::min()),
            rule.filterEnd.value_or(std::numeric_limits<std::int64_t>::max())
        };
    }
    case T::Number:
    {
        if (!rule.filterMinValue.has_value() && !rule.filterMaxValue.has_value())
        {
            return std::nullopt;
        }
        return loglib::RowPredicate{
            std::in_place_type<loglib::NumericRangeRowPredicate>, column, rule.filterMinValue, rule.filterMaxValue
        };
    }
    case T::Boolean:
    {
        if (rule.filterValues.empty())
        {
            return std::nullopt;
        }
        // Case-insensitive decode tolerates hand-edited `"True"` / `"FALSE"`.
        bool includeTrue = false;
        bool includeFalse = false;
        for (const std::string &v : rule.filterValues)
        {
            std::string lower = v;
            std::ranges::transform(lower, lower.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });
            if (lower == "true")
            {
                includeTrue = true;
            }
            else if (lower == "false")
            {
                includeFalse = true;
            }
        }
        if (!includeTrue && !includeFalse)
        {
            return std::nullopt;
        }
        return loglib::RowPredicate{
            std::in_place_type<loglib::BoolRowPredicate>, column, includeTrue, includeFalse
        };
    }
    case T::Enumeration:
    {
        if (rule.filterValues.empty() || table == nullptr)
        {
            return std::nullopt;
        }
        const loglib::EnumDictionary *dictionary = table->ResolveEnumColumn(column).dictionary;
        // Level columns store canonical names (`"Info"`, ...); expand
        // via `LevelRankCache` so a rule saved as `Info` matches
        // `INFO`, `warning`, or any `levelMapping` alias.
        // `EnumRowPredicate` deep-copies the views before these
        // scaffolding vectors go out of scope.
        std::vector<std::string> expandedStorage;
        std::vector<std::string_view> selectedViews;
        const bool isLevelColumn =
            column < columns.size() && columns[column].type == loglib::LogConfiguration::Type::Level;
        if (isLevelColumn)
        {
            const std::vector<loglib::LogLevel> *ranks = table->LevelRankCache(column);
            if (ranks == nullptr || dictionary == nullptr)
            {
                // Column not promoted yet; rebuild on next `Grew`.
                return std::nullopt;
            }
            std::unordered_set<loglib::LogLevel> selectedLevels;
            selectedLevels.reserve(rule.filterValues.size());
            for (const std::string &name : rule.filterValues)
            {
                if (auto level = loglib::ResolveLevel(name, columns[column].levelMapping); level.has_value())
                {
                    selectedLevels.insert(*level);
                }
            }
            expandedStorage.reserve(ranks->size());
            for (std::size_t valueId = 0; valueId < ranks->size(); ++valueId)
            {
                if (selectedLevels.contains((*ranks)[valueId]))
                {
                    expandedStorage.emplace_back(dictionary->Resolve(static_cast<loglib::EnumValueId>(valueId)));
                }
            }
            if (expandedStorage.empty())
            {
                // No dictionary entry matches (e.g. rule targets
                // `Trace` but dict has only `Info`/`Warn`).
                return std::nullopt;
            }
            selectedViews.reserve(expandedStorage.size());
            for (const std::string &v : expandedStorage)
            {
                selectedViews.emplace_back(v);
            }
        }
        else
        {
            selectedViews.reserve(rule.filterValues.size());
            for (const std::string &v : rule.filterValues)
            {
                selectedViews.emplace_back(v);
            }
        }
        return loglib::RowPredicate{
            std::in_place_type<loglib::EnumRowPredicate>,
            column,
            std::span<const std::string_view>(selectedViews),
            dictionary
        };
    }
    case T::String:
    {
        if (!rule.filterString.has_value() || !rule.matchType.has_value())
        {
            return std::nullopt;
        }
        // Reject empty needles for non-`Exactly` match kinds: an
        // empty `Contains` / `Wildcard` / `RegularExpression`
        // matches every row and would silently blank / paint the
        // whole view. `Exactly ""` is genuinely useful (match
        // empty column values), so allow it.
        if (rule.filterString->empty() && *rule.matchType != loglib::LeafRule::Match::Exactly)
        {
            return std::nullopt;
        }
        return loglib::RowPredicate{
            std::in_place_type<loglib::CallbackStringRowPredicate>,
            column,
            MakeStringMatcher(QString::fromStdString(*rule.filterString), *rule.matchType)
        };
    }
    }
    // Unreachable: `switch` is exhaustive with no `default`, so a
    // new `LeafRule::Type` triggers `-Wswitch` here.
    return std::nullopt;
}

namespace
{

/// Recursive compile step. Returns `nullopt` when the sub-tree is
/// **absent** (carries no constraint).
///
/// Absence propagates uniformly: `And` drops absent children (its
/// identity is match-all); `Or` drops them and goes absent when
/// **all** children did; `Not` goes absent when its child did.
///
/// We deliberately treat unresolvable leaves as absent rather than
/// match-none. A leaf goes absent for routine, usually-temporary
/// reasons (column keys not resolved yet, level column not
/// promoted, hand-edited empty payload); folding match-none upwards
/// would blank the view mid-stream with no cue, whereas dropping
/// the clause over-accepts briefly and self-corrects on the next
/// `enumColumnsChanged` rebuild.
///
/// Note the deliberate asymmetry: `NOT <absent>` is absent, not
/// match-all -- otherwise a stale `NOT` would discard every sibling
/// constraint (`svc:auth AND NOT missing:x` would show the whole log).
std::optional<loglib::CompiledFilterExpression> CompileNode(
    const loglib::FilterExpression &expr,
    const std::vector<loglib::LogConfiguration::Column> &columns,
    const loglib::LogTable *table,
    std::vector<std::size_t> &referencedColumns
)
{
    using Node = loglib::FilterExpression;
    return std::visit(
        [&columns, &table, &referencedColumns](const auto &n) -> std::optional<loglib::CompiledFilterExpression> {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, Node::Leaf>)
            {
                const int resolved = ResolveLeafColumnByKeys(n.rule.columnKeys, columns);
                auto predicate = CompileLeaf(n.rule, resolved, columns, table);
                if (!predicate.has_value())
                {
                    return std::nullopt;
                }
                referencedColumns.push_back(static_cast<std::size_t>(resolved));
                loglib::CompiledFilterExpression compiled;
                compiled.node = loglib::CompiledFilterExpression::Leaf{std::move(*predicate)};
                return compiled;
            }
            else if constexpr (std::is_same_v<T, Node::And>)
            {
                loglib::CompiledFilterExpression::And andNode;
                andNode.children.reserve(n.children.size());
                for (const auto &child : n.children)
                {
                    auto compiledChild = CompileNode(child, columns, table, referencedColumns);
                    if (compiledChild.has_value())
                    {
                        andNode.children.push_back(std::move(*compiledChild));
                    }
                    // Absent -> drop (identity of `And` is match-all).
                }
                // Cheap-first: short-circuit reject fires ASAP.
                std::ranges::sort(
                    andNode.children,
                    [](const loglib::CompiledFilterExpression &a, const loglib::CompiledFilterExpression &b) {
                        return a.EstimatedCost() < b.EstimatedCost();
                    }
                );
                int cost = 0;
                for (const auto &child : andNode.children)
                {
                    cost += child.EstimatedCost();
                }
                andNode.estimatedCost = cost;
                loglib::CompiledFilterExpression compiled;
                compiled.node = std::move(andNode);
                return compiled;
            }
            else if constexpr (std::is_same_v<T, Node::Or>)
            {
                loglib::CompiledFilterExpression::Or orNode;
                orNode.children.reserve(n.children.size());
                bool anyChild = false;
                for (const auto &child : n.children)
                {
                    auto compiledChild = CompileNode(child, columns, table, referencedColumns);
                    if (compiledChild.has_value())
                    {
                        orNode.children.push_back(std::move(*compiledChild));
                        anyChild = true;
                    }
                    // Absent -> drop; narrows the `Or` (dropped
                    // branch can no longer admit rows).
                }
                if (!anyChild)
                {
                    // Nothing to disjoin -> `Or` is absent too. See
                    // note above; do not fold to match-none.
                    return std::nullopt;
                }
                // Cheap-first: short-circuit accept fires ASAP.
                std::ranges::sort(
                    orNode.children,
                    [](const loglib::CompiledFilterExpression &a, const loglib::CompiledFilterExpression &b) {
                        return a.EstimatedCost() < b.EstimatedCost();
                    }
                );
                // OR cost = min-of-children (early exit on the
                // cheapest accepting leaf). Ordering hint only.
                int cost = std::numeric_limits<int>::max();
                for (const auto &child : orNode.children)
                {
                    cost = std::min(cost, child.EstimatedCost());
                }
                orNode.estimatedCost = cost;
                loglib::CompiledFilterExpression compiled;
                compiled.node = std::move(orNode);
                return compiled;
            }
            else
            {
                // Not.
                if (n.child == nullptr)
                {
                    // Defensive: `MakeNot` enforces non-null; only a
                    // hand-edited config could land here.
                    return std::nullopt;
                }
                auto compiledChild = CompileNode(*n.child, columns, table, referencedColumns);
                if (!compiledChild.has_value())
                {
                    // Nothing to invert -> absent. NOT absent = absent
                    // (see note above; deliberately not match-all).
                    return std::nullopt;
                }
                loglib::CompiledFilterExpression compiled;
                compiled.node = loglib::CompiledFilterExpression::Not{std::move(*compiledChild)};
                return compiled;
            }
        },
        expr.node
    );
}

} // namespace

loglib::CompiledFilterExpression CompileExpression(
    const loglib::FilterExpression &expression,
    const std::vector<loglib::LogConfiguration::Column> &columns,
    const loglib::LogTable *table
)
{
    std::vector<std::size_t> referencedColumns;
    auto compiled = CompileNode(expression, columns, table, referencedColumns);
    loglib::CompiledFilterExpression result;
    if (compiled.has_value())
    {
        result = std::move(*compiled);
    }
    std::ranges::sort(referencedColumns);
    referencedColumns.erase(std::unique(referencedColumns.begin(), referencedColumns.end()), referencedColumns.end());
    result.referencedColumns = std::move(referencedColumns);
    return result;
}
