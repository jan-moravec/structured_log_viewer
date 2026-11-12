// `loglib` has no Qt in its include chain, so the TBB-before-Qt
// ordering that `app/` needs doesn't apply here.
#include "loglib/query/log_filter.hpp"

#include "loglib/core/compact_log_value.hpp"
#include "loglib/core/log_value.hpp"
#include "loglib/table/log_table.hpp"

#include <oneapi/tbb/blocked_range.h>
#include <oneapi/tbb/enumerable_thread_specific.h>
#include <oneapi/tbb/parallel_for.h>

#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace loglib
{

EnumRowPredicate::EnumRowPredicate(
    size_t columnIndex, std::span<const std::string_view> selectedValues, const EnumDictionary *dictionary
)
    : mColumnIndex(columnIndex)
{
    if (selectedValues.empty())
    {
        mEmptySelection = true;
        return;
    }

    // Dedupe so `mAllResolved` is keyed on distinct values regardless
    // of caller-side dedup, and so the bitset / string-set work stays
    // bounded.
    const std::unordered_set<std::string_view, loglib::TransparentStringHash, loglib::TransparentStringEqual> distinct(
        selectedValues.begin(), selectedValues.end()
    );

    if (dictionary == nullptr)
    {
        mSelectedStrings.reserve(distinct.size());
        for (const std::string_view value : distinct)
        {
            mSelectedStrings.emplace(value);
        }
        return;
    }

    // Indexed by id; ids past `Size()` later go through the
    // past-bitset branch in `MatchesRow`.
    mSelectedIds.assign(static_cast<size_t>(dictionary->Size()), false);
    size_t resolvedCount = 0;
    for (const std::string_view value : distinct)
    {
        const EnumValueId id = dictionary->Find(value);
        if (id == INVALID_ENUM_VALUE_ID)
        {
            // Unresolved -> string-set fallback so the post-rebuild
            // path still matches.
            mSelectedStrings.emplace(value);
            continue;
        }
        const auto idx = static_cast<size_t>(id);
        if (idx >= mSelectedIds.size())
        {
            // Defensive: id past the snapshot we sized against
            // (concurrent dict growth between `Size()` and `Find`).
            // Treat as unresolved.
            mSelectedStrings.emplace(value);
            continue;
        }
        mSelectedIds[idx] = true;
        mFastPathArmed = true;
        ++resolvedCount;
    }
    mAllResolved = resolvedCount == distinct.size();
}

bool EnumRowPredicate::MatchesRow(const LogTable &table, size_t row) const
{
    if (mEmptySelection)
    {
        return false;
    }

    if (mFastPathArmed)
    {
        if (const auto id = table.GetEnumValueId(row, mColumnIndex); id.has_value())
        {
            const auto idx = static_cast<size_t>(*id);
            if (idx < mSelectedIds.size())
            {
                return mSelectedIds[idx];
            }
            if (mAllResolved)
            {
                // Past the bitset, fully resolved -> provably unselected.
                // An id past the bitset can only exist because the
                // dictionary grew after we sized against it. Growth
                // mints ids only for new values, and `mAllResolved`
                // says every selected string already resolved at
                // construction, so the new value cannot be selected.
                return false;
            }
            // Past the bitset with a stale predicate: fall through
            // to the string set. Some selected values were unresolved
            // at construction, so this id may still correspond to one
            // of them once the bytes are compared.
        }
        // Slot isn't a `DictRef`; fall through to the string set.
    }

    if (mSelectedStrings.empty())
    {
        return false;
    }

    const LogValue value = table.GetValue(row, mColumnIndex);
    if (const auto *sv = std::get_if<std::string_view>(&value); sv != nullptr)
    {
        return mSelectedStrings.contains(*sv);
    }
    if (const auto *s = std::get_if<std::string>(&value); s != nullptr)
    {
        return mSelectedStrings.contains(*s);
    }
    return false;
}

TimeRangeRowPredicate::TimeRangeRowPredicate(size_t columnIndex, int64_t begin, int64_t end)
    : mColumnIndex(columnIndex), mBegin(begin), mEnd(end)
{
    assert(begin <= end && end >= 0 && "a time range must not end before the epoch or before it begins");
}

bool TimeRangeRowPredicate::MatchesRow(const LogTable &table, size_t row) const
{
    const CompactLogValue *cell = table.FindCell(row, mColumnIndex);
    if (cell == nullptr)
    {
        return false;
    }
    // The accepted cells must stay in step with loglib::AsEpochMicroseconds: a timestamp, an int64 and an in-range
    // uint64.
    switch (cell->tag)
    {
    case CompactTag::Timestamp:
    case CompactTag::Int64:
    {
        const auto micros = static_cast<int64_t>(cell->payload);
        return micros >= mBegin && micros <= mEnd;
    }
    case CompactTag::Uint64:
    {
        // Reject, rather than wrap, a uint64 past int64 max, as AsEpochMicroseconds does.
        if (cell->payload > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
        {
            return false;
        }
        // A negative begin counts as 0 so that [-1, 100] still matches positive values. The end is never negative.
        const uint64_t lo = mBegin < 0 ? 0U : static_cast<uint64_t>(mBegin);
        const auto hi = static_cast<uint64_t>(mEnd);
        return cell->payload >= lo && cell->payload <= hi;
    }
    default:
        return false;
    }
}

NumericRangeRowPredicate::NumericRangeRowPredicate(
    size_t columnIndex, std::optional<double> minValue, std::optional<double> maxValue
)
    : mColumnIndex(columnIndex), mMin(minValue), mMax(maxValue)
{
    // Collapse NaN bounds to "unbounded": a real NaN bound would
    // reject every row (NaN compares unordered), which is almost
    // never what the caller meant.
    if (mMin.has_value() && std::isnan(*mMin))
    {
        mMin.reset();
    }
    if (mMax.has_value() && std::isnan(*mMax))
    {
        mMax.reset();
    }
}

bool NumericRangeRowPredicate::MatchesRow(const LogTable &table, size_t row) const
{
    const CompactLogValue *cell = table.FindCell(row, mColumnIndex);
    if (cell == nullptr)
    {
        return false;
    }
    double numeric = 0.0;
    switch (cell->tag)
    {
    case CompactTag::Double:
        numeric = std::bit_cast<double>(cell->payload);
        if (std::isnan(numeric))
        {
            return false;
        }
        break;
    case CompactTag::Int64:
        // The cast loses precision past 2^53 (see header).
        numeric = static_cast<double>(static_cast<int64_t>(cell->payload));
        break;
    case CompactTag::Uint64:
        numeric = static_cast<double>(cell->payload);
        break;
    default:
        return false;
    }
    if (mMin.has_value() && numeric < *mMin)
    {
        return false;
    }
    return !(mMax.has_value() && numeric > *mMax);
}

BoolRowPredicate::BoolRowPredicate(size_t columnIndex, bool includeTrue, bool includeFalse)
    : mColumnIndex(columnIndex), mIncludeTrue(includeTrue), mIncludeFalse(includeFalse)
{
}

bool BoolRowPredicate::MatchesRow(const LogTable &table, size_t row) const
{
    if (!mIncludeTrue && !mIncludeFalse)
    {
        return false;
    }
    const CompactLogValue *cell = table.FindCell(row, mColumnIndex);
    if (cell == nullptr || cell->tag != CompactTag::Bool)
    {
        return false;
    }
    return cell->payload != 0 ? mIncludeTrue : mIncludeFalse;
}

CallbackStringRowPredicate::CallbackStringRowPredicate(size_t columnIndex, MatchFn match)
    : mColumnIndex(columnIndex), mMatch(std::move(match))
{
}

bool CallbackStringRowPredicate::MatchesRow(const LogTable &table, size_t row) const
{
    if (!mMatch)
    {
        return false;
    }
    // One-walk path: `GetValueOrFormatted` resolves the slot once and
    // either returns its bytes directly (mmap-aliased / dict-resolved
    // string slots) or formats numeric/time slots into the
    // `thread_local` buffer. The old two-call shape walked the line
    // twice (`GetValue` + `GetFormattedValue`) for every non-string
    // column hit.
    //
    // `thread_local` is safe under `tbb::parallel_for`: each TBB
    // worker has its own buffer. Re-entrancy within a thread is fine
    // because `mMatch` doesn't call back into `MatchesRow`.
    thread_local std::string buffer;
    const std::string_view bytes = table.GetValueOrFormatted(row, mColumnIndex, buffer);
    return mMatch(bytes);
}

// Relative weight for the regex / UTF-8 string branch.  Kept as a
// named constant so cppcoreguidelines-avoid-magic-numbers doesn't
// flag the visit lambda below.
constexpr int STRING_LEAF_COST = 10;

// The `noexcept` on this function is a design guarantee: the
// variant is always populated by construction (predicate
// factories never leave it valueless_by_exception), so
// `std::visit` cannot actually throw `bad_variant_access`.
// clang-tidy's bugprone-exception-escape check can't see that
// invariant; the NOLINT records the argument explicitly.
// NOLINTNEXTLINE(bugprone-exception-escape)
int EstimatedLeafCost(const RowPredicate &predicate) noexcept
{
    // Relative weights (see header). Bool first (cheapest reject
    // fires first in `And`); string / regex last (most expensive).
    return std::visit(
        [](const auto &concrete) noexcept -> int {
            using T = std::decay_t<decltype(concrete)>;
            if constexpr (std::is_same_v<T, BoolRowPredicate>)
            {
                return 1;
            }
            else if constexpr (std::is_same_v<T, EnumRowPredicate>)
            {
                return 2;
            }
            else if constexpr (std::is_same_v<T, TimeRangeRowPredicate>)
            {
                return 3;
            }
            else if constexpr (std::is_same_v<T, NumericRangeRowPredicate>)
            {
                return 4;
            }
            else
            {
                // CallbackString: regex / UTF-8 walk.
                return STRING_LEAF_COST;
            }
        },
        predicate
    );
}

CompiledFilterExpression::Leaf::Leaf(RowPredicate p)
    : predicate(std::move(p)), estimatedCost(EstimatedLeafCost(predicate))
{
}

CompiledFilterExpression::Not::Not(CompiledFilterExpression c)
    : child(std::make_unique<CompiledFilterExpression>(std::move(c))),
      estimatedCost(child ? child->EstimatedCost() + 1 : 1)
{
}

// Same reasoning as `EstimatedLeafCost`: the node variant is always
// populated, so `std::visit` never throws in practice.  The
// arms Leaf/And/Or/Not all return the same `n.estimatedCost` field,
// which trips readability-branch-clone -- collapsed into a single
// generic path to remove the duplication.
// NOLINTNEXTLINE(bugprone-exception-escape)
int CompiledFilterExpression::EstimatedCost() const noexcept
{
    return std::visit([](const auto &n) noexcept -> int { return n.estimatedCost; }, node);
}

bool IsMatchAllCompiled(const CompiledFilterExpression &expression) noexcept
{
    const auto *asAnd = std::get_if<CompiledFilterExpression::And>(&expression.node);
    return asAnd != nullptr && asAnd->children.empty();
}

// Recursive AST walker; misc-no-recursion is suppressed on both the
// entry point and the visitor lambda so the boolean short-circuit
// structure stays legible.
// NOLINTNEXTLINE(misc-no-recursion)
bool EvaluateExpression(const CompiledFilterExpression &expression, const LogTable &table, size_t row)
{
    return std::visit(
        // NOLINTNEXTLINE(misc-no-recursion)
        [&table, row](const auto &node) -> bool {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, CompiledFilterExpression::Leaf>)
            {
                return MatchesRow(node.predicate, table, row);
            }
            else if constexpr (std::is_same_v<T, CompiledFilterExpression::And>)
            {
                // Empty `And` is the identity element -- match all
                // (`std::ranges::all_of` returns true for an empty
                // range, which matches the semantics).
                // NOLINTNEXTLINE(misc-no-recursion)
                return std::ranges::all_of(node.children, [&table, row](const auto &child) {
                    return EvaluateExpression(child, table, row);
                });
            }
            else if constexpr (std::is_same_v<T, CompiledFilterExpression::Or>)
            {
                // Empty `Or` is the identity element -- match none
                // (`std::ranges::any_of` returns false for an empty
                // range, which matches the semantics).
                // NOLINTNEXTLINE(misc-no-recursion)
                return std::ranges::any_of(node.children, [&table, row](const auto &child) {
                    return EvaluateExpression(child, table, row);
                });
            }
            else
            {
                // Not.
                if (node.child == nullptr)
                {
                    // Degenerate: `MakeNot`/`CompileExpression` keep
                    // `child` non-null; only a hand-edited config
                    // could land here. Accept every row so the view
                    // stays visible instead of going blank.
                    return true;
                }
                return !EvaluateExpression(*node.child, table, row);
            }
        },
        expression.node
    );
}

} // namespace loglib
