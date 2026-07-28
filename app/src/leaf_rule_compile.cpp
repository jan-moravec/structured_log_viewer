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
    // Subset match: every rule key must appear in the column's
    // keys. Rules usually carry a single key.
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
    // `HighlightRule::Type` / `HighlightRule::Match` are aliases of
    // `LeafRule::Type` / `LeafRule::Match`, so this is a plain copy
    // of the shared payload -- rendering fields (name, enabled,
    // colours, bold/italic) live on `HighlightRule` and stay behind.
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
        // At least one bound must be finite for the rule to be
        // meaningful. Feed INT64 sentinels for the open side so
        // the per-row visitor stays a simple `>=` / `<=` pair.
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
        // Case-insensitive decode tolerates hand-edited configs
        // (e.g. `"True"`, `"FALSE"`).
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
        // Level columns store canonical names (`"Info"`, ...);
        // expand them to every raw dictionary alias via
        // `LevelRankCache` so a rule saved as `Info` still matches
        // a row parsed from `INFO` or a custom `levelMapping`
        // alias. `EnumRowPredicate`'s constructor deep-copies the
        // views before the scaffolding vectors go out of scope.
        std::vector<std::string> expandedStorage;
        std::vector<std::string_view> selectedViews;
        const bool isLevelColumn =
            column < columns.size() && columns[column].type == loglib::LogConfiguration::Type::Level;
        if (isLevelColumn)
        {
            const std::vector<loglib::LogLevel> *ranks = table->LevelRankCache(column);
            if (ranks == nullptr || dictionary == nullptr)
            {
                // Column not yet promoted -- rebuild on next `Grew`.
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
                // e.g. rule targets `Trace` but dict has only
                // `Info`/`Warn`. Rule matches nothing -- inert.
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
    default:
    {
        if (!rule.filterString.has_value() || !rule.matchType.has_value())
        {
            return std::nullopt;
        }
        // Empty needles paint / hide everything -- almost never
        // intentional. Reject here so hand-authored configs don't
        // silently blank the view.
        if (rule.filterString->empty())
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
}

namespace
{

/// Recursive compile step. Returns `std::nullopt` when the sub-tree
/// collapses to inert (e.g. every leaf was unresolved). The caller
/// then treats a `nullopt` And-child as "match all" (drop) and a
/// `nullopt` Or-child as "match none" (drop). Empty `And` becomes
/// match-all and empty `Or` becomes match-none, which the runtime
/// evaluator handles directly.
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
                    // Inert child in AND -> drop (match-all identity).
                }
                // Cost-order children cheap-first so short-circuit
                // rejects fire ASAP.
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
                    // Inert child in OR -> drop (match-none identity).
                }
                if (!anyChild)
                {
                    // Every child collapsed to inert. In an OR that's
                    // "match none" -- treat the whole OR as inert so
                    // the enclosing AND drops it (a match-none OR
                    // inside an AND would blank the view).
                    return std::nullopt;
                }
                // Cost-order children cheap-first so short-circuit
                // accepts fire ASAP.
                std::ranges::sort(
                    orNode.children,
                    [](const loglib::CompiledFilterExpression &a, const loglib::CompiledFilterExpression &b) {
                        return a.EstimatedCost() < b.EstimatedCost();
                    }
                );
                // OR cost is pessimistic: min-of-children (assume
                // early exit on the cheapest accepting leaf). Doesn't
                // affect correctness -- only sibling ordering.
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
                    // Defensive: `MakeNot`'s constructors enforce
                    // non-null; only a hand-edited config that
                    // survived Glaze validation could land here.
                    // Drop so the parent AND treats us as match-all
                    // rather than propagating the degenerate node.
                    return std::nullopt;
                }
                auto compiledChild = CompileNode(*n.child, columns, table, referencedColumns);
                if (!compiledChild.has_value())
                {
                    // `Not(inert)` conflates a few causes:
                    //   * unresolved column keys (rule targets a
                    //     column that doesn't exist) -- match-all
                    //     is the correct semantic here anyway
                    //     (no row can match the missing column, so
                    //     NOT matches every row);
                    //   * empty payload (e.g. time with no bounds)
                    //     -- also correctly match-all
                    //     (NOT match-none = match-all);
                    //   * level column not yet promoted -- we
                    //     over-accept until the next `Grew` fires
                    //     `enumColumnsChanged` and rebuilds. The
                    //     transient over-acceptance is preferable
                    //     to blanking every row during streaming.
                    // Dropping the whole Not is match-all in the
                    // enclosing AND, which matches all three cases
                    // to the desired steady state.
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
