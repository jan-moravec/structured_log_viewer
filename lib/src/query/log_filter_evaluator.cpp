// The library has no Qt in its include chain, so the TBB-before-Qt include order that the application needs does not
// apply here.
#include "loglib/query/log_filter.hpp"

#include "loglib/core/log_value.hpp"
#include "loglib/table/log_table.hpp"

#include <oneapi/tbb/blocked_range.h>
#include <oneapi/tbb/enumerable_thread_specific.h>
#include <oneapi/tbb/parallel_for.h>
#include <oneapi/tbb/task_arena.h>

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

/** @brief The shape of a filter tree, which ShouldUseBitsetPath uses to pick an evaluation path. */
struct TreeShape
{
    /** @brief The number of leaves, which sets the memory the bitset path needs. */
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

/** @brief The most memory the bitset path may use. A tree that needs more is evaluated row by row. */
constexpr size_t BITSET_MEMORY_CAP_BYTES = size_t{512} * 1024 * 1024;

/**
 * @brief Picks the bitset path or the row-by-row path for a tree of this shape over rowCount rows.
 *
 * A flat And stays row by row: it evaluates a child only on the rows that passed the children before it, which is less
 * work than building a bitset for every leaf. Or and Not reverse that. Or visits every leaf on a rejected row and Not
 * inverts a whole bitset at once, so both gain from combining 64 rows at a time.
 */
[[nodiscard]] bool ShouldUseBitsetPath(const TreeShape &shape, size_t rowCount) noexcept
{
    if (rowCount == 0 || shape.leafCount < 2)
    {
        return false;
    }
    if (!shape.hasOr && !shape.hasNot)
    {
        // A flat And: evaluating row by row and stopping at the first failing child is always cheaper.
        return false;
    }
    // Peak memory is one bitset per leaf, kept for the whole evaluation, plus one bitset per worker inside
    // MaterialiseLeafBitset, freed between leaves.
    const size_t bytesPerBitset = (rowCount / 8) + 1;
    // The workers are those of the task arena the evaluation runs in, at least one.
    const size_t workers = static_cast<size_t>(std::max(1, oneapi::tbb::this_task_arena::max_concurrency()));
    const size_t bytes = (shape.leafCount + workers) * bytesPerBitset;
    return bytes <= BITSET_MEMORY_CAP_BYTES;
}

/** @brief One bit per row, packed into 64-bit words. Bits past the last row are always zero. */
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
        // A write past the last row would set a bit that every operation relies on being zero.
        assert(row < mRowCount);
        mWords[row / WORD_BITS] |= (uint64_t{1} << (row % WORD_BITS));
    }

    [[nodiscard]] bool Test(size_t row) const noexcept
    {
        return (mWords[row / WORD_BITS] & (uint64_t{1} << (row % WORD_BITS))) != 0U;
    }

    void OrInPlace(const RowBitset &other) noexcept
    {
        // All bitsets in one evaluation have the same row count.
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
        // Clear the bits past the last row.
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

    /** @brief The number of set bits, which is the number of accepted rows because bits past the last row are zero. */
    [[nodiscard]] size_t Count() const noexcept
    {
        size_t total = 0;
        for (const uint64_t word : mWords)
        {
            total += static_cast<size_t>(std::popcount(word));
        }
        return total;
    }

    /** @brief Appends the accepted rows to out in ascending order. */
    void CollectInto(std::vector<size_t> &out) const
    {
        // Reserve the number of matches, not the number of rows. A table of 100 million rows would otherwise reserve
        // about 800 MB for a handful of matches.
        out.reserve(out.size() + Count());
        for (size_t wi = 0; wi < mWords.size(); ++wi)
        {
            uint64_t word = mWords[wi];
            while (word != 0U)
            {
                const auto bit = static_cast<unsigned int>(std::countr_zero(word));
                const size_t row = (wi * WORD_BITS) + bit;
                // Every operation that changes bits clears those past the last row, so a set one here is a bug.
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

/**
 * @brief Evaluates one leaf on every row in parallel and returns the rows it accepts as a bitset. Each worker fills its
 * own bitset, and the calling thread combines them at the end.
 */
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

    // Move the first worker's bitset in, which saves clearing and combining one, then combine the rest. With no worker
    // there were no rows, so the result is empty.
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

/**
 * @brief Evaluates a tree from the bitsets of its leaves. leafBitsets holds them in the order CollectLeafsInVisitOrder
 * visits the leaves, and leafCursor walks through them.
 */
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

/**
 * @brief Appends a pointer to each leaf predicate, in the order EvaluateExpressionBitset reads the leaves. Every leaf
 * gets a bitset of its own, also when two leaves are equal.
 */
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
        // Return every row, so callers handle a filter that matches all rows like any other.
        accepted.resize(rowCount);
        std::iota(accepted.begin(), accepted.end(), size_t{0});
        return accepted;
    }

    if (rowCount == 0)
    {
        return accepted;
    }

    // The shape picks the path. Row by row always works; bitsets are faster on trees with Or or Not.
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

    // Row by row: rows are split across workers, and each row walks the tree.
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
