/**
 * @brief Compares and sorts the rows of a table by one column.
 *
 * CompareRows orders two rows by the value in one column, using the rules of the column's type. The sort functions
 * order many rows at once. EnumSortRank precomputes the alphabetical rank of every value of an enumeration dictionary,
 * so an enumeration column sorts by small integers and not by strings. RowOrdering is the public way to use all of
 * this, and it builds the ranks itself, so nothing outside the query module needs to know the rules written here.
 *
 * This is part of the query module and is internal to it.
 */
#pragma once

#include "loglib/config/session_view.hpp"
#include "loglib/core/enum_dictionary.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace loglib
{

class LogTable;

} // namespace loglib

namespace loglib::internal
{

// EnumSortRank stores per-id ranks as uint16_t. Pin the representation against future bumps of MAX_ENUM_VALUES.
static_assert(MAX_ENUM_VALUES <= std::numeric_limits<uint16_t>::max(), "EnumSortRank stores per-id ranks in uint16_t");

/**
 * @brief The alphabetical rank of every value in an enumeration dictionary.
 *
 * CompareRows looks up a rank with RankOf, which saves comparing strings for each comparison. Ranks are built from
 * bytes, so the sort order is the same in every locale. Pickers that list the values for display may sort differently.
 *
 * An id created after the ranks were built gets the rank DictSize, so it sorts after all known values. Build the ranks
 * again when the dictionary grows.
 */
class EnumSortRank
{
public:
    EnumSortRank() = default;

    /** @brief Builds the ranks from the current values of the dictionary. */
    explicit EnumSortRank(const EnumDictionary &dictionary);

    /** @brief The rank of a value id. An id that the ranks do not know gets DictSize, so it sorts last. */
    [[nodiscard]] uint16_t RankOf(EnumValueId id) const noexcept;

    /** @brief The number of values the ranks were built from. */
    [[nodiscard]] uint16_t DictSize() const noexcept;

    /** @brief True if the ranks hold no values. */
    [[nodiscard]] bool Empty() const noexcept
    {
        return mIdToRank.empty();
    }

private:
    /** @brief The alphabetical rank of each enumeration id, indexed by id. */
    std::vector<uint16_t> mIdToRank;
};

/**
 * @brief Two strings that comparing rows formats cell values into, so a steady state allocates nothing.
 *
 * A scratch belongs to one thread at a time. A parallel sort gives each worker its own.
 */
struct CompareScratch
{
    std::string lhs;
    std::string rhs;
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
    const EnumSortRank *rankForEnumColumn,
    CompareScratch &scratch
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
    SortDirection direction,
    const EnumSortRank *rankForEnumColumn
);

/**
 * @brief Returns the part from position begin to position end of the order that SortPermutationByColumn gives, without
 * sorting the rest.
 *
 * The result has end minus begin entries, and entry k is the same as entry begin plus k of SortPermutationByColumn. A
 * query that shows the first rows of a large table, or the last ones, needs only that part. Both positions are limited
 * to the number of rows, and begin to end. Number, boolean, time and string columns select the part without sorting
 * everything. The other column types build the whole order and cut it.
 */
[[nodiscard]] std::vector<size_t> SortPermutationWindowByColumn(
    const LogTable &table,
    std::span<const size_t> logRows,
    size_t columnIndex,
    SortDirection direction,
    size_t begin,
    size_t end,
    const EnumSortRank *rankForEnumColumn
);

} // namespace loglib::internal
