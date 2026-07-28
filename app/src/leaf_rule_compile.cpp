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
    {
        if (!rule.filterString.has_value() || !rule.matchType.has_value())
        {
            return std::nullopt;
        }
        // Empty needles: whether this is meaningful depends on the
        // match kind.
        //   - `Contains` matches every string, `Wildcard` (with a
        //     glob) and `RegularExpression` (with a pattern) either
        //     match every string or degenerate to trivial matchers.
        //     Rejecting them keeps a hand-authored `col:""` from
        //     silently painting / hiding every row.
        //   - `Exactly ""` is a specific, useful query -- match
        //     genuinely empty column values. Rejecting it would
        //     silently drop the leaf on load, so let it through.
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
    // Unreachable: the switch above is exhaustive and carries no
    // `default:`, so adding a `LeafRule::Type` is a `-Wswitch` error
    // here rather than a silent reinterpretation of the new type as
    // a `String` leaf (which is what the old `case T::String:
    // default:` fallthrough did).
    return std::nullopt;
}

namespace
{

/// Recursive compile step. Returns `std::nullopt` when the sub-tree
/// is **absent** -- it carries no constraint the evaluator can apply.
///
/// "Absent" is a single, uniform rule rather than a truth value, and
/// every parent applies it the same way: drop the child, and if that
/// leaves the parent with nothing of its own, report the parent as
/// absent too. So `And` drops absent children (its identity is
/// match-all), `Or` drops them and goes absent when *all* of its
/// children did, and `Not` goes absent when its child did.
///
/// The alternative -- treating an unresolvable leaf as match-none and
/// propagating that through the tree -- is rejected deliberately. A
/// leaf goes absent for reasons that are routine and usually
/// temporary: column keys that don't resolve yet, a `Level` column
/// that hasn't been promoted, an empty payload from a hand-edited
/// config. Folding match-none upwards would blank the table mid-
/// stream and give the user no cue as to why, whereas dropping the
/// clause over-accepts for a moment and then self-corrects on the
/// next `enumColumnsChanged` rebuild. Over-accepting is recoverable;
/// an empty view that looks like data loss is not.
///
/// Note the asymmetry this creates on purpose: `NOT <absent>` is
/// absent, *not* match-all. Reading `NOT` as "invert match-none" and
/// widening to match-all would discard every sibling constraint in
/// the enclosing `And` -- `svc:auth AND NOT missing:x` would show the
/// whole log rather than just the `svc:auth` rows.
///
/// Independently of all this, an empty `And` is match-all and an
/// empty `Or` is match-none; the runtime evaluator handles both.
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
                    // Absent child -> drop; `And`'s identity is match-all.
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
                    // Absent child -> drop, keeping the surviving
                    // alternatives. Note this narrows the `Or`: the
                    // dropped branch can no longer admit rows.
                }
                if (!anyChild)
                {
                    // Nothing left to disjoin, so the `Or` carries no
                    // constraint of its own and is absent in turn.
                    // Reporting match-none here instead would blank
                    // the view (see the note on `CompileNode`).
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
                    // Nothing to invert, so the node is absent.
                    return std::nullopt;
                }
                auto compiledChild = CompileNode(*n.child, columns, table, referencedColumns);
                if (!compiledChild.has_value())
                {
                    // There is no constraint to invert, so the `Not`
                    // is absent as well. Deliberately *not* match-all
                    // -- see the asymmetry note on `CompileNode`.
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
