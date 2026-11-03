// `loglib` has no Qt in its include chain, so the TBB-before-Qt
// ordering that `app/` needs doesn't apply here.
#include "loglib/query/log_filter.hpp"

#include "loglib/core/log_value.hpp"
#include "loglib/table/log_table.hpp"

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

namespace
{

/// Tree summary consumed by `ShouldUseBitsetPath`.
struct TreeShape
{
    /// Number of leaves; drives the bitset memory estimate.
    size_t leafCount = 0;
    bool hasOr = false;
    bool hasNot = false;
};

// NOLINTNEXTLINE(misc-no-recursion)
void CollectShape(const CompiledFilterExpression &expr, TreeShape &shape)
{
    std::visit(
        // NOLINTNEXTLINE(misc-no-recursion)
        [&shape](const auto &node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, CompiledFilterExpression::Leaf>)
            {
                ++shape.leafCount;
            }
            else if constexpr (std::is_same_v<T, CompiledFilterExpression::And>)
            {
                for (const auto &child : node.children)
                {
                    CollectShape(child, shape);
                }
            }
            else if constexpr (std::is_same_v<T, CompiledFilterExpression::Or>)
            {
                shape.hasOr = true;
                for (const auto &child : node.children)
                {
                    CollectShape(child, shape);
                }
            }
            else
            {
                shape.hasNot = true;
                if (node.child != nullptr)
                {
                    CollectShape(*node.child, shape);
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
    const size_t workers = std::max<size_t>(
        1, oneapi::tbb::global_control::active_value(oneapi::tbb::global_control::max_allowed_parallelism)
    );
    const size_t bytes = (shape.leafCount + workers) * bytesPerBitset;
    return bytes <= BITSET_MEMORY_CAP_BYTES;
}

/// Packed word-sized bitset over `[0, rowCount)`. Bit-set / test /
/// AND / OR / NOT are all inline.
class RowBitset
{
public:
    RowBitset() = default;
    explicit RowBitset(size_t rowCount)
        : mRowCount(rowCount), mWords(WordCount(rowCount), 0U)
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
/// @p leafBitsets, which are in the order `CollectLeafsInVisitOrder`
/// visits leaves. Recurses over And / Or / Not.
// NOLINTNEXTLINE(misc-no-recursion)
RowBitset EvaluateExpressionBitset(
    const CompiledFilterExpression &expr, const std::vector<RowBitset> &leafBitsets, size_t &leafCursor, size_t rowCount
)
{
    return std::visit(
        // NOLINTNEXTLINE(misc-no-recursion)
        [&leafBitsets, &leafCursor, rowCount](const auto &node) -> RowBitset {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, CompiledFilterExpression::Leaf>)
            {
                return leafBitsets[leafCursor++];
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
                RowBitset acc = EvaluateExpressionBitset(node.children.front(), leafBitsets, leafCursor, rowCount);
                for (size_t i = 1; i < node.children.size(); ++i)
                {
                    const RowBitset next =
                        EvaluateExpressionBitset(node.children[i], leafBitsets, leafCursor, rowCount);
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
                RowBitset acc = EvaluateExpressionBitset(node.children.front(), leafBitsets, leafCursor, rowCount);
                for (size_t i = 1; i < node.children.size(); ++i)
                {
                    const RowBitset next =
                        EvaluateExpressionBitset(node.children[i], leafBitsets, leafCursor, rowCount);
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
                RowBitset inner = EvaluateExpressionBitset(*node.child, leafBitsets, leafCursor, rowCount);
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
    CollectShape(expression, shape);

    if (ShouldUseBitsetPath(shape, rowCount))
    {
        std::vector<const RowPredicate *> orderedLeaves;
        orderedLeaves.reserve(shape.leafCount);
        CollectLeafsInVisitOrder(expression, orderedLeaves);

        std::vector<RowBitset> leafBitsets;
        leafBitsets.reserve(orderedLeaves.size());
        for (const RowPredicate *predicate : orderedLeaves)
        {
            leafBitsets.push_back(MaterialiseLeafBitset(*predicate, table, rowCount));
        }

        size_t leafCursor = 0;
        const RowBitset resultBitset = EvaluateExpressionBitset(expression, leafBitsets, leafCursor, rowCount);
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
