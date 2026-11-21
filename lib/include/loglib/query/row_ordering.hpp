/**
 * @brief Sort order of the rows of a table.
 *
 * A RowOrdering holds a column and a direction and describes how a table's rows are ordered. It does not depend on any
 * user interface type, so an interface converts its own sort order at the boundary.
 *
 * This is part of the query module.
 */
#pragma once

#include "loglib/query/log_compare.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace loglib
{

class LogTable;

/** @brief The direction of a sort. An application that has its own sort order type converts it to this one. */
enum class SortDirection
{
    Ascending,
    Descending,
};

/**
 * @brief Bundles a column, a direction and optional enumeration ranks into one way of ordering the rows of a LogTable.
 *
 * It compares and sorts through CompareRows and SortPermutationByColumn, so it always orders like they do. Rows whose
 * cells are equal are ordered by a tie key from the caller, ascending, as SortPermutationByColumn orders them by their
 * input position.
 */
class RowOrdering
{
public:
    /**
     * @brief Orders by the column at columnIndex in the table's column layout. Give rankForEnumColumn, the borrowed
     * ranks of the column's enumeration values, to sort an enumeration column by rank. Without it such a column is
     * sorted by its text.
     */
    RowOrdering(
        std::size_t columnIndex, SortDirection direction, const EnumDictRank *rankForEnumColumn = nullptr
    ) noexcept;

    /** @brief The column this ordering sorts on. */
    [[nodiscard]] std::size_t ColumnIndex() const noexcept
    {
        return mColumnIndex;
    }

    /** @brief The direction this ordering sorts in. */
    [[nodiscard]] SortDirection Direction() const noexcept
    {
        return mDirection;
    }

    /** @brief True if the direction is ascending. */
    [[nodiscard]] bool Ascending() const noexcept
    {
        return mDirection == SortDirection::Ascending;
    }

    /** @brief The enumeration ranks given at construction, or nullptr. */
    [[nodiscard]] const EnumDictRank *RankForEnumColumn() const noexcept
    {
        return mRankForEnumColumn;
    }

    /** @brief Compares two rows of the table on this column like CompareRows. The direction is not applied. */
    [[nodiscard]] int Compare(const LogTable &table, std::size_t lhsRow, std::size_t rhsRow) const;

    /**
     * @brief True if the left row belongs before the right row in display order. The direction is applied. Rows with
     * equal cells are ordered by their tie keys, ascending, so the order is stable.
     */
    [[nodiscard]] bool LessThan(
        const LogTable &table, std::size_t lhsRow, std::size_t rhsRow, std::size_t lhsTieKey, std::size_t rhsTieKey
    ) const;

    /** @brief Sorts logRows as SortPermutationByColumn does. The result holds indices into logRows in display order. */
    [[nodiscard]] std::vector<std::size_t> Permute(const LogTable &table, std::span<const std::size_t> logRows) const;

    /**
     * @brief The part of the Permute result from position begin to position end, without sorting the rest. It works
     * like SortPermutationWindowByColumn.
     */
    [[nodiscard]] std::vector<std::size_t> PermuteWindow(
        const LogTable &table, std::span<const std::size_t> logRows, std::size_t begin, std::size_t end
    ) const;

private:
    std::size_t mColumnIndex = 0;
    SortDirection mDirection = SortDirection::Ascending;
    const EnumDictRank *mRankForEnumColumn = nullptr;
};

} // namespace loglib
