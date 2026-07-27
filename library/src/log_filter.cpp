// `loglib` has no Qt in its include chain, so the TBB-before-Qt
// ordering that `app/` needs doesn't apply here.
#include "loglib/log_filter.hpp"

#include "loglib/log_table.hpp"
#include "loglib/log_value.hpp"

#include <oneapi/tbb/blocked_range.h>
#include <oneapi/tbb/enumerable_thread_specific.h>
#include <oneapi/tbb/parallel_for.h>

#include <algorithm>
#include <bit>
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

int EstimatedLeafCost(const RowPredicate &predicate) noexcept
{
    // Numbers are relative -- see the header table. Bool is
    // cheapest so its short-circuit reject fires first in AND;
    // string / regex is most expensive so it's tried last.
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
                // CallbackStringRowPredicate -- regex / UTF-8 walk.
                return 10;
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
    : child(std::make_unique<CompiledFilterExpression>(std::move(c)))
{
    estimatedCost = child ? child->EstimatedCost() + 1 : 1;
}

int CompiledFilterExpression::EstimatedCost() const noexcept
{
    return std::visit(
        [](const auto &n) noexcept -> int {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, Leaf>)
            {
                return n.estimatedCost;
            }
            else if constexpr (std::is_same_v<T, Not>)
            {
                return n.estimatedCost;
            }
            else
            {
                // And / Or: prebaked at compile time.
                return n.estimatedCost;
            }
        },
        node
    );
}

bool IsMatchAllCompiled(const CompiledFilterExpression &expression) noexcept
{
    const auto *asAnd = std::get_if<CompiledFilterExpression::And>(&expression.node);
    return asAnd != nullptr && asAnd->children.empty();
}

bool EvaluateExpression(const CompiledFilterExpression &expression, const LogTable &table, size_t row)
{
    return std::visit(
        [&table, row](const auto &node) -> bool {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, CompiledFilterExpression::Leaf>)
            {
                return MatchesRow(node.predicate, table, row);
            }
            else if constexpr (std::is_same_v<T, CompiledFilterExpression::And>)
            {
                // Empty `And` is the identity element -- match all.
                for (const auto &child : node.children)
                {
                    if (!EvaluateExpression(child, table, row))
                    {
                        return false;
                    }
                }
                return true;
            }
            else if constexpr (std::is_same_v<T, CompiledFilterExpression::Or>)
            {
                // Empty `Or` is the identity element -- match none.
                for (const auto &child : node.children)
                {
                    if (EvaluateExpression(child, table, row))
                    {
                        return true;
                    }
                }
                return false;
            }
            else
            {
                // Not.
                if (node.child == nullptr)
                {
                    // Empty `Not` = NOT (match-none) = match-all.
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

/// Walk @p node's tree and count the unique leaves + whether any
/// regex / wildcard / OR / NOT is present. Used to gate the bitset
/// path below.
struct TreeShape
{
    size_t leafCount = 0;
    bool hasRegexOrWildcard = false;
    bool hasOr = false;
    bool hasNot = false;
};

void CollectShape(const CompiledFilterExpression &expr, TreeShape &shape)
{
    std::visit(
        [&shape](const auto &node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, CompiledFilterExpression::Leaf>)
            {
                ++shape.leafCount;
                if (std::holds_alternative<CallbackStringRowPredicate>(node.predicate))
                {
                    shape.hasRegexOrWildcard = true;
                }
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

/// Bitset-materialisation eligibility. Trees that would waste the
/// bitset (flat `And` of one or two cheap leaves) stay on the visit
/// path; complex or OR-heavy trees switch. The memory cap is
/// generous but hard -- catastrophic bitsets fall back to visit.
constexpr size_t BITSET_MEMORY_CAP_BYTES = size_t{512} * 1024 * 1024;

[[nodiscard]] bool ShouldUseBitsetPath(const TreeShape &shape, size_t rowCount) noexcept
{
    if (rowCount == 0 || shape.leafCount == 0)
    {
        return false;
    }
    const bool complexShape = (shape.leafCount >= 2 && (shape.hasRegexOrWildcard || shape.hasOr || shape.hasNot)) ||
                              shape.leafCount >= 4;
    if (!complexShape)
    {
        return false;
    }
    const size_t bytes = (rowCount / 8 + 1) * shape.leafCount;
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
        mWords[row / WORD_BITS] |= (uint64_t{1} << (row % WORD_BITS));
    }

    [[nodiscard]] bool Test(size_t row) const noexcept
    {
        return (mWords[row / WORD_BITS] & (uint64_t{1} << (row % WORD_BITS))) != 0U;
    }

    void OrInPlace(const RowBitset &other) noexcept
    {
        for (size_t i = 0; i < mWords.size(); ++i)
        {
            mWords[i] |= other.mWords[i];
        }
    }

    void AndInPlace(const RowBitset &other) noexcept
    {
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

    /// Extract accepted rows in ascending order.
    void CollectInto(std::vector<size_t> &out) const
    {
        out.reserve(out.size() + mRowCount);
        for (size_t wi = 0; wi < mWords.size(); ++wi)
        {
            uint64_t word = mWords[wi];
            while (word != 0U)
            {
                const auto bit = static_cast<unsigned int>(std::countr_zero(word));
                const size_t row = wi * WORD_BITS + bit;
                if (row >= mRowCount)
                {
                    break;
                }
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

    RowBitset combined(rowCount);
    for (const auto &worker : workerBitsets)
    {
        combined.OrInPlace(worker);
    }
    return combined;
}

/// Evaluate @p expr against the pre-materialised leaf bitsets in
/// @p leafBitsets, in the order returned by
/// `CollectLeafsInVisitOrder`. Recurses over And / Or / Not.
RowBitset EvaluateExpressionBitset(
    const CompiledFilterExpression &expr, const std::vector<RowBitset> &leafBitsets, size_t &leafCursor, size_t rowCount
)
{
    return std::visit(
        [&leafBitsets, &leafCursor, rowCount](const auto &node) -> RowBitset {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, CompiledFilterExpression::Leaf>)
            {
                RowBitset copy(rowCount);
                copy.OrInPlace(leafBitsets[leafCursor]);
                ++leafCursor;
                return copy;
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
                    // Empty Not = match-all.
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

/// Walk the tree in the same order `EvaluateExpressionBitset` will
/// consume leaves and append pointers to the leaf `RowPredicate`s.
/// Simpler than deduplicating leaves at compile time; the caller
/// then materialises one bitset per pointer, potentially
/// duplicating work if the same leaf appears twice. Leaf dedup is
/// a follow-up optimisation.
void CollectLeafsInVisitOrder(const CompiledFilterExpression &expr, std::vector<const RowPredicate *> &out)
{
    std::visit(
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
        // Identity case: hand back `[0, rowCount)` so callers can
        // share one code path with the filtered case.
        accepted.resize(rowCount);
        std::iota(accepted.begin(), accepted.end(), size_t{0});
        return accepted;
    }

    if (rowCount == 0)
    {
        return accepted;
    }

    // Shape analysis picks the evaluator per rebuild. The visit
    // path is always safe; the bitset path is a perf win for
    // complex trees.
    TreeShape shape;
    CollectShape(expression, shape);

    if (ShouldUseBitsetPath(shape, rowCount))
    {
        // Bitset-materialisation path.
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
