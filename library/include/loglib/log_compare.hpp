#pragma once

#include "loglib/enum_dictionary.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace loglib
{

class LogTable;

// `EnumDictRank` stores per-id ranks as `uint16_t`. Pin the
// representation against future bumps of `MAX_ENUM_VALUES`.
static_assert(MAX_ENUM_VALUES <= std::numeric_limits<uint16_t>::max(), "EnumDictRank stores per-id ranks in uint16_t");

/// Precomputed alphabetic-rank table over an `EnumDictionary`.
/// `CompareRows` calls `RankOf(id)` to avoid per-compare string
/// compares. Byte-wise build keeps the sort deterministic across
/// locales (the picker keeps its own locale-aware order for display).
///
/// Ids past `DictSize()` (minted after the last rebuild) get rank
/// `DictSize()` so they sort after every known value.
class EnumDictRank
{
public:
    EnumDictRank() = default;

    /// Build from @p dictionary's current values. Safe to repeat.
    explicit EnumDictRank(const EnumDictionary &dictionary);

    [[nodiscard]] uint16_t RankOf(EnumValueId id) const noexcept;

    [[nodiscard]] uint16_t DictSize() const noexcept;

    [[nodiscard]] bool Empty() const noexcept
    {
        return mIdToRank.empty();
    }

private:
    /// `mIdToRank[i]` is the alphabetic rank of `EnumValueId{i}`.
    std::vector<uint16_t> mIdToRank;
};

/// Three-way row comparator over a single column (<0, 0, >0).
///
/// Dispatch by `LogConfiguration::Type`:
///   - `Boolean`              -- bool compare; `false < true`.
///   - `Integer`              -- int64_t compare; uint64_t > INT64_MAX
///                               clamps to INT64_MAX.
///   - `Floating` / `Number`  -- double compare; NaN sinks to tail.
///   - `Time`                 -- microseconds-since-epoch compare.
///   - `Enumeration`          -- `EnumDictRank` lookup, byte-wise
///                               string fallback when @p rankForEnumColumn
///                               is null.
///   - `String` / `Any` -- byte-wise string compare.
///
/// Tail-bucket invariant (ascending): every slot unrepresentable in
/// the column's logical type sorts strictly greater than every
/// representable slot and equal to every other unrepresentable slot.
/// Membership per type:
///   - `Boolean`              -- monostate, non-bool slots.
///   - `Integer`              -- monostate, NaN. ±inf / out-of-range
///                               doubles clamp rather than joining.
///   - `Floating` / `Number`  -- monostate, non-numeric slots.
///   - `Time`                 -- monostate, non-microsecond slots.
///   - `Enumeration`          -- every non-`DictRef` slot (monostate,
///                               unpromoted-string, wrong-type,
///                               over-cap-length). Matches
///                               `SortPermutationByColumn`'s rank-
///                               sentinel collapse so the bulk re-sort
///                               and the streaming comparator agree
///                               on placement.
///   - String types           -- just monostate.
/// Tail members compare equal pairwise. To put monostate at the head
/// instead, negate the return.
[[nodiscard]] int CompareRows(
    const LogTable &table,
    size_t lhsRow,
    size_t rhsRow,
    size_t columnIndex,
    const EnumDictRank *rankForEnumColumn = nullptr
);

/// Sort permutation for @p logRows by their slot in @p columnIndex.
/// Returns a vector `P` of size `logRows.size()` such that
/// `logRows[P[k]]` is the row at rank `k`. Stable: ties resolve to
/// input-index order, so callers carrying parallel arrays (source
/// rows, view ranges) can reapply the permutation and keep
/// insertion-order tie-break.
///
/// Pass @p rankForEnumColumn when @p columnIndex is an `Enumeration`
/// column; the lib then pre-materialises a `uint16_t` rank per row in
/// parallel and the comparator collapses to a branch-free integer
/// compare. Without a rank the comparator dispatches through
/// `CompareRows`, paying the per-call slot resolution cost --
/// noticeably slower on enum columns but correct for the non-enum
/// types (`Time`, `Integer`, `Floating`, string).
///
/// Threading: pre-materialisation runs under `tbb::parallel_for`, the
/// sort under `tbb::parallel_sort`. Both are read-only against
/// @p table; `LogTable::GetEnumValueId` and `CompareRows` must be
/// safe to call concurrently (today's implementations are).
[[nodiscard]] std::vector<size_t> SortPermutationByColumn(
    const LogTable &table,
    std::span<const size_t> logRows,
    size_t columnIndex,
    bool ascending,
    const EnumDictRank *rankForEnumColumn = nullptr
);

} // namespace loglib
