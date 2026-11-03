/**
 * @brief Compares two rows of a column for sorting.
 *
 * CompareRows orders two rows by the value in one column, using the rules of the column's type. EnumDictRank
 * precomputes the alphabetical rank of every value in an enumeration dictionary, so sorting an enumeration column
 * compares small integers instead of strings.
 *
 * This is part of the query module.
 */
#pragma once

#include "loglib/core/enum_dictionary.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace loglib
{

class LogTable;

// EnumDictRank stores per-id ranks as uint16_t. Pin the
// representation against future bumps of MAX_ENUM_VALUES.
static_assert(MAX_ENUM_VALUES <= std::numeric_limits<uint16_t>::max(), "EnumDictRank stores per-id ranks in uint16_t");

/**
 * @brief The alphabetical rank of every value in an enumeration dictionary.
 *
 * CompareRows looks up a rank with RankOf, which saves comparing strings for each comparison. Ranks are built from
 * bytes, so the sort order is the same in every locale. Pickers that list the values for display may sort differently.
 *
 * An id created after the table was built gets the rank DictSize, so it sorts after all known values.
 */
class EnumDictRank
{
public:
    EnumDictRank() = default;

    /**
     * @brief Builds the ranks from the current values of the dictionary. Building again after the dictionary changed
     * is fine.
     */
    explicit EnumDictRank(const EnumDictionary &dictionary);

    [[nodiscard]] uint16_t RankOf(EnumValueId id) const noexcept;

    [[nodiscard]] uint16_t DictSize() const noexcept;

    [[nodiscard]] bool Empty() const noexcept
    {
        return mIdToRank.empty();
    }

private:
    /** @brief The alphabetical rank of each enumeration id, indexed by id. */
    std::vector<uint16_t> mIdToRank;
};

/**
 * @brief Compares two rows by one column. The result is negative, zero or positive.
 *
 * How the values are compared depends on the column type:
 * - Boolean: false before true.
 * - Integer: as int64_t, and a uint64_t above INT64_MAX counts as INT64_MAX.
 * - Floating and Number: as doubles.
 * - Time: as microseconds since the epoch.
 * - Enumeration: by the rank in rankForEnumColumn, or by bytes when that is null.
 * - String and Any: by bytes.
 *
 * Cells that do not fit the column's type sort after all cells that do, in ascending order, and are equal to one
 * another. Negate the result to put them first. Which cells do not fit:
 * - Boolean: empty cells and cells that are not booleans.
 * - Integer: empty cells and NaN. Infinite and out-of-range doubles are clamped and still fit.
 * - Floating and Number: empty cells and cells that are not numbers. NaN counts as not fitting.
 * - Time: empty cells and cells that are not microsecond times.
 * - Enumeration: every cell that is not encoded, which means empty cells, plain strings and values of the wrong type. A
 *   bulk sort by SortPermutationByColumn puts them in the same place, so sorting in bulk and comparing while streaming
 *   agree.
 * - String types: empty cells only.
 */
[[nodiscard]] int CompareRows(
    const LogTable &table,
    size_t lhsRow,
    size_t rhsRow,
    size_t columnIndex,
    const EnumDictRank *rankForEnumColumn = nullptr
);

/**
 * @brief Sorts rows by one column and returns the order as a permutation.
 *
 * The result P has one entry per row, and logRows[P[k]] is the row at position k. The sort is stable: rows that compare
 * equal keep their input order, so a caller with parallel arrays can apply the same permutation to them and keep the
 * original order for ties.
 *
 * For an Enumeration column pass rankForEnumColumn. The ranks are then looked up once per row, in parallel, and the
 * sort compares plain integers. Without a rank the sort calls CompareRows for every comparison, which is much slower on
 * enumeration columns and correct for all other types.
 *
 * Lookups and the sort run on several threads, and the table is only read.
 */
[[nodiscard]] std::vector<size_t> SortPermutationByColumn(
    const LogTable &table,
    std::span<const size_t> logRows,
    size_t columnIndex,
    bool ascending,
    const EnumDictRank *rankForEnumColumn = nullptr
);

} // namespace loglib
