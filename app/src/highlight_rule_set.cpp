#include "highlight_rule_set.hpp"

#include "log_string_matcher.hpp"

#include <loglib/log_configuration.hpp>
#include <loglib/log_filter.hpp>
#include <loglib/log_level.hpp>
#include <loglib/log_table.hpp>

#include <QString>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

/// One rule's compiled state. Held as a pimpl-like unique_ptr in
/// `mCompiled` so the enclosing `HighlightRuleSet::CompiledRule`
/// forward declaration stays out of the public header.
///
/// `predicate` is the runtime match callable. All backing storage
/// (dictionary aliases for enum rules, regex objects for string
/// rules, ...) lives *inside* the predicate itself; see
/// `EnumRowPredicate::mSelectedStrings` and
/// `CallbackStringRowPredicate`'s captured lambda for the two
/// non-trivial cases. `CompileRule` builds any transient scaffolding
/// (e.g. an expanded canonical-level alias list) on the local stack
/// and lets the predicate's constructor copy what it needs before
/// the scaffolding goes out of scope.
///
/// Explicit constructor because `loglib::RowPredicate` (a `variant`
/// over non-default-constructible predicates) can't be default-
/// initialised, so aggregate-then-assign doesn't compile.
struct HighlightRuleSet::CompiledRule
{
    loglib::RowPredicate predicate;

    explicit CompiledRule(loglib::RowPredicate p) : predicate(std::move(p))
    {
    }
};

namespace
{

/// Sentinel written into `mRowMatch` for "no rule matched this row".
constexpr std::int16_t NO_MATCH = -1;

} // namespace

HighlightRuleSet::HighlightRuleSet(QObject *parent) : QObject(parent)
{
}

HighlightRuleSet::~HighlightRuleSet() = default;

const std::vector<loglib::LogConfiguration::HighlightRule> &HighlightRuleSet::Rules() const noexcept
{
    return mRules;
}

std::optional<std::size_t> HighlightRuleSet::LastMatchFor(std::size_t sourceRow) const noexcept
{
    if (sourceRow >= mRowMatch.size())
    {
        return std::nullopt;
    }
    const std::int16_t match = mRowMatch[sourceRow];
    if (match < 0)
    {
        return std::nullopt;
    }
    return static_cast<std::size_t>(match);
}

std::size_t HighlightRuleSet::InactiveCount() const noexcept
{
    return mInactiveCount;
}

bool HighlightRuleSet::Empty() const noexcept
{
    return mRules.empty();
}

bool HighlightRuleSet::HasActiveRules() const noexcept
{
    return mActiveCount > 0;
}

const std::vector<int> &HighlightRuleSet::ResolvedColumnsForTest() const noexcept
{
    return mResolvedColumn;
}

int HighlightRuleSet::ResolveColumnByKeys(
    const std::vector<std::string> &keys, const std::vector<loglib::LogConfiguration::Column> &columns
) noexcept
{
    if (keys.empty())
    {
        return -1;
    }
    // A column matches iff its `keys` contain every entry in the rule's
    // key list. In practice each rule stores a single key, so the
    // inner loop bottoms out at one comparison per column.
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

std::optional<HighlightRuleSet::CompiledRule> HighlightRuleSet::CompileRule(
    const loglib::LogConfiguration::HighlightRule &rule,
    int resolvedColumn,
    const std::vector<loglib::LogConfiguration::Column> &columns,
    const loglib::LogTable *table
)
{
    if (!rule.enabled || resolvedColumn < 0)
    {
        return std::nullopt;
    }
    const std::size_t column = static_cast<std::size_t>(resolvedColumn);
    using RuleType = loglib::LogConfiguration::HighlightRule::Type;
    switch (rule.type)
    {
    case RuleType::Time:
    {
        // Same convention as filters: `nullopt` = unbounded; feed
        // int64 sentinels so the per-row visitor stays a simple
        // pair-compare. At least one bound must be finite for the
        // rule to be meaningful.
        if (!rule.filterBegin.has_value() && !rule.filterEnd.has_value())
        {
            return std::nullopt;
        }
        return CompiledRule{loglib::RowPredicate{
            std::in_place_type<loglib::TimeRangeRowPredicate>,
            column,
            rule.filterBegin.value_or(std::numeric_limits<std::int64_t>::min()),
            rule.filterEnd.value_or(std::numeric_limits<std::int64_t>::max())
        }};
    }
    case RuleType::Number:
    {
        if (!rule.filterMinValue.has_value() && !rule.filterMaxValue.has_value())
        {
            return std::nullopt;
        }
        return CompiledRule{loglib::RowPredicate{
            std::in_place_type<loglib::NumericRangeRowPredicate>,
            column,
            rule.filterMinValue,
            rule.filterMaxValue
        }};
    }
    case RuleType::Boolean:
    {
        if (rule.filterValues.empty())
        {
            return std::nullopt;
        }
        // Case-insensitive decode; tolerates hand-edited configs
        // (mirrors `DecodeBooleanFilterSides` in `main_window.cpp`).
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
        return CompiledRule{loglib::RowPredicate{
            std::in_place_type<loglib::BoolRowPredicate>, column, includeTrue, includeFalse
        }};
    }
    case RuleType::Enumeration:
    {
        if (rule.filterValues.empty() || table == nullptr)
        {
            return std::nullopt;
        }
        const loglib::EnumDictionary *dictionary = table->ResolveEnumColumn(column).dictionary;
        // Level columns store canonical names (`"Info"`, ...) in
        // `filterValues`; expand them to every raw dictionary alias
        // via the LevelRankCache so a rule saved as `Info` still
        // matches a row parsed from the raw string `INFO`. Mirrors
        // the level branch in `MainWindow::UpdateFilters`.
        //
        // The scaffolding (`expandedStorage` + `selectedViews`)
        // lives on the local stack; `EnumRowPredicate`'s constructor
        // deep-copies the string_views into its own owned storage
        // (`mSelectedStrings` / `mSelectedIds`) before this scope
        // exits, so there's no dangling-view hazard.
        std::vector<std::string> expandedStorage;
        std::vector<std::string_view> selectedViews;
        const bool isLevelColumn =
            column < columns.size() && columns[column].type == loglib::LogConfiguration::Type::Level;
        if (isLevelColumn)
        {
            const std::vector<loglib::LogLevel> *ranks = table->LevelRankCache(column);
            if (ranks == nullptr || dictionary == nullptr)
            {
                // Column not yet populated; leave the rule inert
                // until the next `RebindColumns` pass observes a
                // populated dictionary.
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
                // Legit (e.g. rule targets `Trace` but only
                // `Info`/`Warn` slots exist). Reject every row --
                // handled below by the empty predicate short-circuit.
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
        return CompiledRule{loglib::RowPredicate{
            std::in_place_type<loglib::EnumRowPredicate>,
            column,
            std::span<const std::string_view>(selectedViews),
            dictionary
        }};
    }
    case RuleType::String:
    default:
    {
        if (!rule.filterString.has_value() || !rule.matchType.has_value())
        {
            return std::nullopt;
        }
        return CompiledRule{loglib::RowPredicate{
            std::in_place_type<loglib::CallbackStringRowPredicate>,
            column,
            MakeStringMatcher(QString::fromStdString(*rule.filterString), *rule.matchType)
        }};
    }
    }
}

void HighlightRuleSet::RecompileAll(
    const std::vector<loglib::LogConfiguration::Column> &columns, const loglib::LogTable *table
)
{
    mCompiled.clear();
    mResolvedColumn.clear();
    mCompiled.reserve(mRules.size());
    mResolvedColumn.reserve(mRules.size());
    std::size_t inactive = 0;
    std::size_t active = 0;
    for (const auto &rule : mRules)
    {
        const int resolved = ResolveColumnByKeys(rule.columnKeys, columns);
        mResolvedColumn.push_back(resolved);
        auto compiled = CompileRule(rule, resolved, columns, table);
        if (compiled.has_value())
        {
            mCompiled.push_back(std::make_unique<CompiledRule>(std::move(*compiled)));
            ++active;
        }
        else
        {
            mCompiled.push_back(nullptr);
            ++inactive;
        }
    }
    mInactiveCount = inactive;
    mActiveCount = active;
}

void HighlightRuleSet::EvaluateRows(const loglib::LogTable &table, std::size_t first, std::size_t last)
{
    if (mCompiled.empty() || first > last || last >= mRowMatch.size())
    {
        return;
    }
    // Last-match-wins: iterate rules from the last back to the
    // first; the first accepting rule for a row wins that row.
    //
    // Per-row inner loop keeps the per-rule state (predicate + its
    // captured lambda-state) hot in the cache. A per-rule outer
    // loop with row-in-inner would be friendlier to CPU caches
    // when rules match sparsely, but sparse matches also mean
    // little inner work, and the reverse ordering is easier to
    // audit. Revisit under the perf harness if needed.
    for (std::size_t row = first; row <= last; ++row)
    {
        std::int16_t match = NO_MATCH;
        for (std::size_t i = mCompiled.size(); i-- > 0;)
        {
            const auto &compiled = mCompiled[i];
            if (compiled == nullptr)
            {
                continue;
            }
            if (loglib::MatchesRow(compiled->predicate, table, row))
            {
                match = static_cast<std::int16_t>(i);
                break;
            }
        }
        mRowMatch[row] = match;
    }
}

void HighlightRuleSet::RebuildAllMatches(const loglib::LogTable &table)
{
    const std::size_t rowCount = table.RowCount();
    mRowMatch.assign(rowCount, NO_MATCH);
    if (mCompiled.empty() || mActiveCount == 0 || rowCount == 0)
    {
        return;
    }
    EvaluateRows(table, 0, rowCount - 1);
}

void HighlightRuleSet::SetRules(
    std::vector<loglib::LogConfiguration::HighlightRule> rules,
    const std::vector<loglib::LogConfiguration::Column> &columns,
    const loglib::LogTable *table
)
{
    mRules = std::move(rules);
    RecompileAll(columns, table);
    // Rebuild the match cache BEFORE broadcasting `rulesChanged`.
    // Previously the emit order was `rulesChanged` → rebuild →
    // `matchesChanged`, which meant any synchronous slot on
    // `rulesChanged` that touched `LastMatchFor` would observe stale
    // per-row indices from the prior rule set. No consumer does that
    // today, but the invariant "when `rulesChanged` fires, the
    // per-row cache already reflects the new rules" is cheap and
    // future-proof.
    if (table != nullptr)
    {
        RebuildAllMatches(*table);
    }
    else
    {
        mRowMatch.clear();
    }
    emit rulesChanged(mInactiveCount);
    emit matchesChanged();
}

void HighlightRuleSet::RebindColumns(
    const std::vector<loglib::LogConfiguration::Column> &columns, const loglib::LogTable *table
)
{
    if (mRules.empty())
    {
        // Fast path: nothing to recompile. Still reset the caches
        // so a Reset() sequence (rules cleared, then RebindColumns
        // called by MainWindow) leaves us in the expected state.
        mCompiled.clear();
        mResolvedColumn.clear();
        mInactiveCount = 0;
        mActiveCount = 0;
        return;
    }
    RecompileAll(columns, table);
    // Match cache first, then broadcast -- see `SetRules` for the
    // ordering rationale.
    if (table != nullptr)
    {
        RebuildAllMatches(*table);
    }
    emit rulesChanged(mInactiveCount);
    if (table != nullptr)
    {
        emit matchesChanged();
    }
}

void HighlightRuleSet::OnRowsAppended(
    const loglib::LogTable &table, std::size_t firstNewRow, std::size_t lastNewRow
)
{
    if (mRules.empty() || mActiveCount == 0)
    {
        // Grow the cache so `LastMatchFor` stays in sync with the
        // table size even when no rule is active.
        const std::size_t rowCount = table.RowCount();
        if (rowCount > mRowMatch.size())
        {
            mRowMatch.resize(rowCount, NO_MATCH);
        }
        return;
    }
    if (firstNewRow > lastNewRow)
    {
        return;
    }
    // Grow the cache before evaluating; `EvaluateRows` asserts on
    // this via its bounds check.
    if (lastNewRow >= mRowMatch.size())
    {
        mRowMatch.resize(lastNewRow + 1, NO_MATCH);
    }
    EvaluateRows(table, firstNewRow, lastNewRow);
    emit matchesChanged();
}

void HighlightRuleSet::OnRowsEvicted(std::size_t first, std::size_t last)
{
    if (first > last || mRowMatch.empty() || first >= mRowMatch.size())
    {
        return;
    }
    // Clamp against the current cache size so a spurious over-run
    // eviction (which shouldn't happen -- `LogModel::AppendBatch`
    // always evicts a contiguous prefix -- but we defend anyway)
    // doesn't degenerate into a wild-erase.
    const std::size_t clampedLast = std::min(last, mRowMatch.size() - 1);
    mRowMatch.erase(mRowMatch.begin() + static_cast<std::ptrdiff_t>(first),
                    mRowMatch.begin() + static_cast<std::ptrdiff_t>(clampedLast + 1));
    // Deliberately no `matchesChanged` emit: Qt views react to the
    // upstream `rowsRemoved` signal on their own; a redundant
    // `dataChanged` broadcast here would just cost repaints.
}

void HighlightRuleSet::ClearMatches()
{
    if (mRowMatch.empty())
    {
        return;
    }
    mRowMatch.clear();
    emit matchesChanged();
}
