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

    // Work on distinct values, so that the all-resolved flag counts each value once and the bitset and string set stay
    // small whatever the caller passes.
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

    // Indexed by id. An id past the dictionary's current size takes the past-the-bitset branch of MatchesRow.
    mSelectedIds.assign(static_cast<size_t>(dictionary->Size()), false);
    size_t resolvedCount = 0;
    for (const std::string_view value : distinct)
    {
        const EnumValueId id = dictionary->Find(value);
        if (id == INVALID_ENUM_VALUE_ID)
        {
            // A value the dictionary does not know yet goes to the string set, so it still matches after a rebuild.
            mSelectedStrings.emplace(value);
            continue;
        }
        const auto idx = static_cast<size_t>(id);
        if (idx >= mSelectedIds.size())
        {
            // The dictionary grew between the size read and the lookup, so treat the value as unresolved.
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
                // An id past the bitset belongs to a value added after construction. Every selected value was
                // resolved then, so the new value is not selected.
                return false;
            }
            // Some selected values were unresolved at construction, so this new id may be one of them. The string
            // set decides.
        }
        // The cell is not a dictionary reference, so the string set decides.
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
    // GetValueOrFormatted finds the cell once. It returns the bytes of a string cell directly and formats any other
    // cell into the buffer. Each worker thread has its own buffer, and the matcher never calls back into MatchesRow,
    // so the buffer is not reused while it is read.
    thread_local std::string buffer;
    const std::string_view bytes = table.GetValueOrFormatted(row, mColumnIndex, buffer);
    return mMatch(bytes);
}

// The relative cost of a string leaf, which is the most expensive kind.
constexpr int STRING_LEAF_COST = 10;

// The predicate variant always holds a value, so the visit cannot throw.
// NOLINTNEXTLINE(bugprone-exception-escape)
int EstimatedLeafCost(const RowPredicate &predicate) noexcept
{
    // Relative weights, described in the header. A boolean leaf is cheapest, so it runs first in an And, and a string
    // leaf runs last.
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

// The node variant always holds a value, so the visit cannot throw. Every node kind keeps its cost in the same field.
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

// Walks the tree recursively, with the short-circuit of And and Or.
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
                // An empty And accepts every row, as all_of does for an empty range.
                // NOLINTNEXTLINE(misc-no-recursion)
                return std::ranges::all_of(node.children, [&table, row](const auto &child) {
                    return EvaluateExpression(child, table, row);
                });
            }
            else if constexpr (std::is_same_v<T, CompiledFilterExpression::Or>)
            {
                // An empty Or accepts no row, as any_of does for an empty range.
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
                    // The compiler never builds a Not without a child, so only a hand-edited configuration gets
                    // here. Accept every row, so the view does not go blank.
                    return true;
                }
                return !EvaluateExpression(*node.child, table, row);
            }
        },
        expression.node
    );
}

} // namespace loglib
