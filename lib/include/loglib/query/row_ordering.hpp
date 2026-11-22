/**
 * @brief Sort order of the rows of a table.
 *
 * A RowOrdering is a column and a direction applied to one LogTable. It compares two rows, says which of two rows comes
 * first, and sorts lists of rows. It holds everything a sort needs, including the ranks of an enumeration column, so a
 * caller cannot forget to prepare them and always gets the fast path. It depends on no user interface type, so an
 * interface converts its own sort order at the boundary.
 *
 * This is part of the query module.
 */
#pragma once

#include "loglib/config/session_view.hpp"

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace loglib
{

class EnumDictionary;
class LogTable;

namespace internal
{

class EnumSortRank;

} // namespace internal

/**
 * @brief Orders the rows of a LogTable by one column in one direction.
 *
 * The ordering refers to the table and does not own it, so the table must outlive the ordering. For an Enumeration
 * column the ordering builds the alphabetical rank of the column's values when it is constructed. Build a new ordering
 * when the dictionary of the column has grown, which IsOutdated tells. Rows whose cells are equal are ordered by a tie
 * key from the caller, ascending, as Permute orders them by their input position. Comparing and sorting give the same
 * order.
 *
 * All members are const and only read the table. Compare and LessThan may be called from one thread at a time for each
 * thread that uses them, and Permute uses several threads inside.
 */
class RowOrdering
{
public:
    /**
     * @brief Orders by the column at the given index of the table's column layout. A column that does not exist orders
     * all rows as equal.
     */
    RowOrdering(const LogTable &table, std::size_t column, SortDirection direction);

    /** @brief The column this ordering sorts on. */
    [[nodiscard]] std::size_t Column() const noexcept
    {
        return mColumn;
    }

    /** @brief The direction this ordering sorts in. */
    [[nodiscard]] SortDirection Direction() const noexcept
    {
        return mDirection;
    }

    /**
     * @brief True if the table's enumeration dictionary for the column is no longer the one the ranks were built from,
     * because it grew or was replaced. Build a new ordering then. Always false for other column types.
     */
    [[nodiscard]] bool IsOutdated() const;

    /** @brief Compares two rows of the table on this column. The result is negative, zero or positive. The direction is
     * not applied. */
    [[nodiscard]] int Compare(std::size_t lhsRow, std::size_t rhsRow) const;

    /**
     * @brief True if the left row belongs before the right row in display order. The direction is applied. Rows with
     * equal cells are ordered by their tie keys, ascending, so the order is stable.
     */
    [[nodiscard]] bool LessThan(
        std::size_t lhsRow, std::size_t rhsRow, std::size_t lhsTieKey, std::size_t rhsTieKey
    ) const;

    /** @brief Sorts the rows and returns the order as a permutation. Entry k of the result is an index into rows. */
    [[nodiscard]] std::vector<std::size_t> Permute(std::span<const std::size_t> rows) const;

    /**
     * @brief The part of the Permute result from position begin to position end, without sorting the rest. Both
     * positions are limited to the number of rows, and begin to end.
     */
    [[nodiscard]] std::vector<std::size_t> PermuteWindow(
        std::span<const std::size_t> rows, std::size_t begin, std::size_t end
    ) const;

private:
    const LogTable *mTable;
    std::size_t mColumn;
    SortDirection mDirection;

    /** @brief The ranks of an Enumeration column, or null for any other column. Shared, so that copies are cheap. */
    std::shared_ptr<const internal::EnumSortRank> mRank;

    /** @brief The dictionary the ranks were built from, and its size then. Used by IsOutdated. */
    const EnumDictionary *mRankSource = nullptr;
    std::size_t mRankSize = 0;
};

} // namespace loglib
