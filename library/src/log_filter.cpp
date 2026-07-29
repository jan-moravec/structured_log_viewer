// `loglib` has no Qt in its include chain, so the TBB-before-Qt
// ordering that `app/` needs doesn't apply here.
#include "loglib/log_filter.hpp"

#include "loglib/log_table.hpp"
#include "loglib/log_value.hpp"

#include <oneapi/tbb/blocked_range.h>
#include <oneapi/tbb/enumerable_thread_specific.h>
#include <oneapi/tbb/global_control.h>
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
    const std::unordered_set<std::string_view, internal::TransparentStringHash, internal::TransparentStringEqual>
        distinct(selectedValues.begin(), selectedValues.end());

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
}

bool TimeRangeRowPredicate::MatchesRow(const LogTable &table, size_t row) const
{
    if (mBegin > mEnd)
    {
        return false;
    }
    const LogValue value = table.GetValue(row, mColumnIndex);
    // Slot acceptance set must stay in lockstep with
    // `loglib::AsEpochMicroseconds`: `TimeStamp`, `int64_t`,
    // in-range `uint64_t`.
    return std::visit(
        [this](const auto &alt) -> bool {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, TimeStamp>)
            {
                const int64_t ts = alt.time_since_epoch().count();
                return ts >= mBegin && ts <= mEnd;
            }
            else if constexpr (std::is_same_v<T, int64_t>)
            {
                return alt >= mBegin && alt <= mEnd;
            }
            else if constexpr (std::is_same_v<T, uint64_t>)
            {
                // Reject (rather than wrap) `uint64_t` past
                // `int64_t::max` so this stays in sync with
                // `AsEpochMicroseconds`.
                if (alt > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
                {
                    return false;
                }
                // Clamp negative bounds to 0 so e.g. `[-1, 100]`
                // still matches positive values.
                const uint64_t lo = mBegin < 0 ? 0U : static_cast<uint64_t>(mBegin);
                const uint64_t hi = mEnd < 0 ? 0U : static_cast<uint64_t>(mEnd);
                return alt >= lo && alt <= hi;
            }
            else
            {
                return false;
            }
        },
        value
    );
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
    const LogValue value = table.GetValue(row, mColumnIndex);
    return std::visit(
        [this](const auto &alt) -> bool {
            using T = std::decay_t<decltype(alt)>;
            // Single exit after the `if constexpr` chain to avoid
            // MSVC C4702 (a branch returning early would make the
            // common tail unreachable for that instantiation).
            std::optional<double> asDouble;
            if constexpr (std::is_same_v<T, double>)
            {
                if (!std::isnan(alt))
                {
                    asDouble = alt;
                }
            }
            else if constexpr (std::is_same_v<T, int64_t> || std::is_same_v<T, uint64_t>)
            {
                // Cast loses precision past 2^53 (see header).
                asDouble = static_cast<double>(alt);
            }
            if (!asDouble.has_value())
            {
                return false;
            }
            const double numeric = *asDouble;
            if (mMin.has_value() && numeric < *mMin)
            {
                return false;
            }
            if (mMax.has_value() && numeric > *mMax)
            {
                return false;
            }
            return true;
        },
        value
    );
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
    const LogValue value = table.GetValue(row, mColumnIndex);
    if (const auto *b = std::get_if<bool>(&value); b != nullptr)
    {
        return *b ? mIncludeTrue : mIncludeFalse;
    }
    return false;
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

namespace
{

/// Tree summary consumed by `ShouldUseBitsetPath`.
struct TreeShape
{
    /// Total leaf occurrences (duplicates counted).
    size_t leafCount = 0;
    /// Unique leaves by predicate identity; drives the bitset
    /// memory estimate (the path dedups by predicate).
    size_t uniqueLeafCount = 0;
    bool hasOr = false;
    bool hasNot = false;
};

// NOLINTNEXTLINE(misc-no-recursion)
void CollectShape(
    const CompiledFilterExpression &expr, TreeShape &shape, std::vector<const RowPredicate *> &seenPredicates
)
{
    std::visit(
        // NOLINTNEXTLINE(misc-no-recursion)
        [&shape, &seenPredicates](const auto &node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, CompiledFilterExpression::Leaf>)
            {
                ++shape.leafCount;
                if (std::ranges::find(seenPredicates, &node.predicate) == seenPredicates.end())
                {
                    seenPredicates.push_back(&node.predicate);
                    ++shape.uniqueLeafCount;
                }
            }
            else if constexpr (std::is_same_v<T, CompiledFilterExpression::And>)
            {
                for (const auto &child : node.children)
                {
                    CollectShape(child, shape, seenPredicates);
                }
            }
            else if constexpr (std::is_same_v<T, CompiledFilterExpression::Or>)
            {
                shape.hasOr = true;
                for (const auto &child : node.children)
                {
                    CollectShape(child, shape, seenPredicates);
                }
            }
            else
            {
                shape.hasNot = true;
                if (node.child != nullptr)
                {
                    CollectShape(*node.child, shape, seenPredicates);
                }
            }
        },
        expr.node
    );
}

/// Hard cap on bitset memory; over-cap trees fall back to visit.
constexpr size_t BITSET_MEMORY_CAP_BYTES = size_t{512} * 1024 * 1024;

/// Pick between the bitset and visit paths for @p shape over
/// @p rowCount rows.
///
/// Flat `And` stays on visit: short-circuit only evaluates child N
/// on rows that passed 1..N-1, strictly less work than materialising
/// every leaf. `Or`/`Not` flip the trade -- `Or` still visits every
/// leaf on rejected rows, `Not` inverts a whole column at once, and
/// repeated leaves cost one shared bitset -- and both benefit from
/// word-parallel folding.
[[nodiscard]] bool ShouldUseBitsetPath(const TreeShape &shape, size_t rowCount) noexcept
{
    if (rowCount == 0 || shape.leafCount < 2)
    {
        return false;
    }
    if (!shape.hasOr && !shape.hasNot)
    {
        // Flat `And`: short-circuit evaluation strictly dominates.
        return false;
    }
    // Peak memory = `unique * bytesPerBitset` (kept for the whole
    // evaluation) + `workers * bytesPerBitset` (per-worker bitsets
    // inside `MaterialiseLeafBitset`, torn down between leaves).
    const size_t bytesPerBitset = (rowCount / 8) + 1;
    // `active_value` reads the live parallelism ceiling; floor at 1
    // for single-thread test runs.
    const size_t workers =
        std::max<size_t>(1, oneapi::tbb::global_control::active_value(
                                oneapi::tbb::global_control::max_allowed_parallelism));
    const size_t bytes = (shape.uniqueLeafCount + workers) * bytesPerBitset;
    return bytes <= BITSET_MEMORY_CAP_BYTES;
}

/// Packed word-sized bitset over `[0, rowCount)`. Bit-set / test /
/// AND / OR / NOT are all inline.
class RowBitset
{
public:
    RowBitset() = default;
    explicit RowBitset(size_t rowCount) : mRowCount(rowCount), mWords(WordCount(rowCount), 0U)
    {
    }

    [[nodiscard]] size_t RowCount() const noexcept
    {
        return mRowCount;
    }
    [[nodiscard]] size_t WordSize() const noexcept
    {
        return mWords.size();
    }

    void Set(size_t row) noexcept
    {
        // Out-of-range writes would clobber tail bits and break
        // the `MaskTail` invariant every op relies on.
        assert(row < mRowCount);
        mWords[row / WORD_BITS] |= (uint64_t{1} << (row % WORD_BITS));
    }

    [[nodiscard]] bool Test(size_t row) const noexcept
    {
        return (mWords[row / WORD_BITS] & (uint64_t{1} << (row % WORD_BITS))) != 0U;
    }

    void OrInPlace(const RowBitset &other) noexcept
    {
        // All bitsets in one evaluation share a `rowCount`.
        assert(mWords.size() == other.mWords.size());
        for (size_t i = 0; i < mWords.size(); ++i)
        {
            mWords[i] |= other.mWords[i];
        }
    }

    void AndInPlace(const RowBitset &other) noexcept
    {
        assert(mWords.size() == other.mWords.size());
        for (size_t i = 0; i < mWords.size(); ++i)
        {
            mWords[i] &= other.mWords[i];
        }
    }

    void FillTrue() noexcept
    {
        std::ranges::fill(mWords, ~uint64_t{0});
        // Mask off the tail bits past `mRowCount`.
        MaskTail();
    }

    void InvertInPlace() noexcept
    {
        for (auto &w : mWords)
        {
            w = ~w;
        }
        MaskTail();
    }

    /// Number of set bits. Tail bits past `mRowCount` are always
    /// masked off, so this is exactly the accepted-row count.
    [[nodiscard]] size_t Count() const noexcept
    {
        size_t total = 0;
        for (const uint64_t word : mWords)
        {
            total += static_cast<size_t>(std::popcount(word));
        }
        return total;
    }

    /// Extract accepted rows in ascending order.
    void CollectInto(std::vector<size_t> &out) const
    {
        // Reserve by popcount, not `mRowCount`: reserving per-row
        // would allocate ~800 MB per 100 M-row table for a
        // handful of matches.
        out.reserve(out.size() + Count());
        for (size_t wi = 0; wi < mWords.size(); ++wi)
        {
            uint64_t word = mWords[wi];
            while (word != 0U)
            {
                const auto bit = static_cast<unsigned int>(std::countr_zero(word));
                const size_t row = (wi * WORD_BITS) + bit;
                // Every mutating op must call `MaskTail`, so a set
                // bit past `mRowCount` here would be a bug.
                assert(row < mRowCount);
                out.push_back(row);
                word &= word - 1U;
            }
        }
    }

private:
    static constexpr size_t WORD_BITS = 64U;

    [[nodiscard]] static size_t WordCount(size_t rowCount) noexcept
    {
        return (rowCount + WORD_BITS - 1U) / WORD_BITS;
    }

    void MaskTail() noexcept
    {
        if (mWords.empty())
        {
            return;
        }
        const size_t tail = mRowCount % WORD_BITS;
        if (tail == 0)
        {
            return;
        }
        const uint64_t mask = (uint64_t{1} << tail) - 1U;
        mWords.back() &= mask;
    }

    size_t mRowCount = 0;
    std::vector<uint64_t> mWords;
};

/// Materialise @p predicate's accept-set into a packed bitset in
/// parallel. Each worker owns a private bitset; the main thread
/// OR-coalesces at the end.
RowBitset MaterialiseLeafBitset(const RowPredicate &predicate, const LogTable &table, size_t rowCount)
{
    tbb::enumerable_thread_specific<RowBitset> workerBitsets{[rowCount] { return RowBitset(rowCount); }};
    tbb::parallel_for(
        tbb::blocked_range<size_t>(0, rowCount),
        [&predicate, &table, &workerBitsets](const tbb::blocked_range<size_t> &range) {
            auto &local = workerBitsets.local();
            for (size_t row = range.begin(); row != range.end(); ++row)
            {
                if (MatchesRow(predicate, table, row))
                {
                    local.Set(row);
                }
            }
        }
    );

    // Seed from the first worker (move: skip zero-init + full OR),
    // fold the rest. No workers = no rows, return empty.
    RowBitset combined;
    bool seeded = false;
    for (auto &worker : workerBitsets)
    {
        if (!seeded)
        {
            combined = std::move(worker);
            seeded = true;
        }
        else
        {
            combined.OrInPlace(worker);
        }
    }
    if (!seeded)
    {
        combined = RowBitset(rowCount);
    }
    return combined;
}

/// Evaluate @p expr against the pre-materialised leaf bitsets in
/// @p leafBitsets, indexed by the mapping in @p leafSlots (built
/// by `CollectLeafsInVisitOrder`). Recurses over And / Or / Not.
// NOLINTNEXTLINE(misc-no-recursion)
RowBitset EvaluateExpressionBitset(
    const CompiledFilterExpression &expr,
    const std::vector<RowBitset> &leafBitsets,
    const std::vector<std::size_t> &leafSlots,
    size_t &leafCursor,
    size_t rowCount
)
{
    return std::visit(
        // NOLINTNEXTLINE(misc-no-recursion)
        [&leafBitsets, &leafSlots, &leafCursor, rowCount](const auto &node) -> RowBitset {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, CompiledFilterExpression::Leaf>)
            {
                // `leafSlots` maps each visit-position to a shared
                // physical bitset slot so a leaf appearing multiple
                // times materialises once.
                const std::size_t slot = leafSlots[leafCursor];
                ++leafCursor;
                return leafBitsets[slot];
            }
            else if constexpr (std::is_same_v<T, CompiledFilterExpression::And>)
            {
                if (node.children.empty())
                {
                    // Empty And = match-all: fill and return.
                    RowBitset all(rowCount);
                    all.FillTrue();
                    return all;
                }
                RowBitset acc =
                    EvaluateExpressionBitset(node.children.front(), leafBitsets, leafSlots, leafCursor, rowCount);
                for (size_t i = 1; i < node.children.size(); ++i)
                {
                    const RowBitset next =
                        EvaluateExpressionBitset(node.children[i], leafBitsets, leafSlots, leafCursor, rowCount);
                    acc.AndInPlace(next);
                }
                return acc;
            }
            else if constexpr (std::is_same_v<T, CompiledFilterExpression::Or>)
            {
                if (node.children.empty())
                {
                    // Empty Or = match-none: zero-initialised.
                    return RowBitset(rowCount);
                }
                RowBitset acc =
                    EvaluateExpressionBitset(node.children.front(), leafBitsets, leafSlots, leafCursor, rowCount);
                for (size_t i = 1; i < node.children.size(); ++i)
                {
                    const RowBitset next =
                        EvaluateExpressionBitset(node.children[i], leafBitsets, leafSlots, leafCursor, rowCount);
                    acc.OrInPlace(next);
                }
                return acc;
            }
            else
            {
                // Not.
                if (node.child == nullptr)
                {
                    // See the visit path above: degenerate state,
                    // accept every row for consistency.
                    RowBitset all(rowCount);
                    all.FillTrue();
                    return all;
                }
                RowBitset inner =
                    EvaluateExpressionBitset(*node.child, leafBitsets, leafSlots, leafCursor, rowCount);
                inner.InvertInPlace();
                return inner;
            }
        },
        expr.node
    );
}

/// Walk @p expr in the same order `EvaluateExpressionBitset`
/// consumes leaves and append pointers to leaf predicates. The
/// caller dedups by pointer (identity is stable for the tree's
/// lifetime) so a repeated leaf materialises once. Structurally
/// identical leaves compiled independently still cost one bitset
/// each -- follow-up if it ever matters.
// NOLINTNEXTLINE(misc-no-recursion)
void CollectLeafsInVisitOrder(const CompiledFilterExpression &expr, std::vector<const RowPredicate *> &out)
{
    std::visit(
        // NOLINTNEXTLINE(misc-no-recursion)
        [&out](const auto &node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, CompiledFilterExpression::Leaf>)
            {
                out.push_back(&node.predicate);
            }
            else if constexpr (std::is_same_v<T, CompiledFilterExpression::And>)
            {
                for (const auto &child : node.children)
                {
                    CollectLeafsInVisitOrder(child, out);
                }
            }
            else if constexpr (std::is_same_v<T, CompiledFilterExpression::Or>)
            {
                for (const auto &child : node.children)
                {
                    CollectLeafsInVisitOrder(child, out);
                }
            }
            else
            {
                if (node.child != nullptr)
                {
                    CollectLeafsInVisitOrder(*node.child, out);
                }
            }
        },
        expr.node
    );
}

} // namespace

std::vector<size_t> FilterAcceptedRows(const LogTable &table, const CompiledFilterExpression &expression)
{
    const size_t rowCount = table.RowCount();
    std::vector<size_t> accepted;

    if (IsMatchAllCompiled(expression))
    {
        // Identity case: hand back every row so callers share one
        // code path with the filtered case.
        accepted.resize(rowCount);
        std::iota(accepted.begin(), accepted.end(), size_t{0});
        return accepted;
    }

    if (rowCount == 0)
    {
        return accepted;
    }

    // Shape drives evaluator choice. Visit is always safe; bitset
    // is the perf win on complex trees.
    TreeShape shape;
    std::vector<const RowPredicate *> seenPredicates;
    CollectShape(expression, shape, seenPredicates);

    if (ShouldUseBitsetPath(shape, rowCount))
    {
        std::vector<const RowPredicate *> orderedLeaves;
        orderedLeaves.reserve(shape.leafCount);
        CollectLeafsInVisitOrder(expression, orderedLeaves);

        // Dedup by predicate pointer: one bitset per unique
        // predicate. `leafSlots` maps visit-position -> physical slot.
        std::vector<std::size_t> leafSlots;
        leafSlots.reserve(orderedLeaves.size());
        std::vector<const RowPredicate *> uniquePredicates;
        uniquePredicates.reserve(orderedLeaves.size());
        for (const RowPredicate *predicate : orderedLeaves)
        {
            const auto it = std::ranges::find(uniquePredicates, predicate);
            if (it == uniquePredicates.end())
            {
                leafSlots.push_back(uniquePredicates.size());
                uniquePredicates.push_back(predicate);
            }
            else
            {
                leafSlots.push_back(
                    static_cast<std::size_t>(std::distance(uniquePredicates.begin(), it))
                );
            }
        }

        std::vector<RowBitset> leafBitsets;
        leafBitsets.reserve(uniquePredicates.size());
        for (const RowPredicate *predicate : uniquePredicates)
        {
            leafBitsets.push_back(MaterialiseLeafBitset(*predicate, table, rowCount));
        }

        size_t leafCursor = 0;
        const RowBitset resultBitset =
            EvaluateExpressionBitset(expression, leafBitsets, leafSlots, leafCursor, rowCount);
        resultBitset.CollectInto(accepted);
        return accepted;
    }

    // Visit path: parallel-for over rows, each row walks the tree.
    tbb::enumerable_thread_specific<std::vector<size_t>> buckets;
    tbb::parallel_for(
        tbb::blocked_range<size_t>(0, rowCount),
        [&table, &expression, &buckets](const tbb::blocked_range<size_t> &range) {
            auto &local = buckets.local();
            local.reserve(local.size() + range.size());
            for (size_t row = range.begin(); row != range.end(); ++row)
            {
                if (EvaluateExpression(expression, table, row))
                {
                    local.push_back(row);
                }
            }
        }
    );

    size_t total = 0;
    for (const auto &bucket : buckets)
    {
        total += bucket.size();
    }
    accepted.reserve(total);
    for (const auto &bucket : buckets)
    {
        accepted.insert(accepted.end(), bucket.begin(), bucket.end());
    }
    std::ranges::sort(accepted);
    return accepted;
}

} // namespace loglib
