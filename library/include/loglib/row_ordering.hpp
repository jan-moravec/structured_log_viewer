#pragma once

#include "loglib/log_compare.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace loglib
{

class LogTable;

/**
 * @brief Sort direction for `RowOrdering`.
 *
 * Not a Qt type; the GUI converts `Qt::SortOrder` at the proxy
 * boundary.
 */
enum class SortDirection
{
    Ascending,
    Descending,
};

/**
 * @brief Packages one-column compare and permute for a `LogTable`.
 *
 * Composes `CompareRows`, `EnumDictRank`, and
 * `SortPermutationByColumn`. Does not fork a second comparator.
 * Equal cells keep a caller-supplied tie key in ascending order,
 * matching `SortPermutationByColumn`'s input-index tie-break.
 */
class RowOrdering
{
public:
    /**
     * @brief Constructs an ordering over @p columnIndex.
     *
     * @param columnIndex Index into `LogConfiguration::columns`.
     * @param direction Display order.
     * @param rankForEnumColumn Precomputed enum ranks, or `nullptr`.
     *     Required for the `Enumeration` fast path in `Permute`.
     */
    RowOrdering(
        std::size_t columnIndex, SortDirection direction, const EnumDictRank *rankForEnumColumn = nullptr
    ) noexcept;

    /** @brief Returns the configured column index. */
    [[nodiscard]] std::size_t ColumnIndex() const noexcept
    {
        return mColumnIndex;
    }

    /** @brief Returns the configured direction. */
    [[nodiscard]] SortDirection Direction() const noexcept
    {
        return mDirection;
    }

    /** @brief Returns whether the direction is `Ascending`. */
    [[nodiscard]] bool Ascending() const noexcept
    {
        return mDirection == SortDirection::Ascending;
    }

    /** @brief Returns the borrowed enum-rank table, or `nullptr`. */
    [[nodiscard]] const EnumDictRank *RankForEnumColumn() const noexcept
    {
        return mRankForEnumColumn;
    }

    /**
     * @brief Three-way-compares two log rows on this column.
     *
     * Delegates to `CompareRows`. Does not apply direction.
     *
     * @param table Table that owns the rows.
     * @param lhsRow Left log-row index.
     * @param rhsRow Right log-row index.
     * @return Negative, zero, or positive as `CompareRows`.
     */
    [[nodiscard]] int Compare(const LogTable &table, std::size_t lhsRow, std::size_t rhsRow) const;

    /**
     * @brief Returns whether @p lhsRow sorts before @p rhsRow.
     *
     * Applies @p direction to `CompareRows`. Equal cells compare
     * @p lhsTieKey and @p rhsTieKey ascending, matching
     * `SortPermutationByColumn`.
     *
     * @param table Table that owns the rows.
     * @param lhsRow Left log-row index.
     * @param rhsRow Right log-row index.
     * @param lhsTieKey Stable tie key for the left row.
     * @param rhsTieKey Stable tie key for the right row.
     * @return `true` when the left row belongs earlier in display order.
     */
    [[nodiscard]] bool LessThan(
        const LogTable &table,
        std::size_t lhsRow,
        std::size_t rhsRow,
        std::size_t lhsTieKey,
        std::size_t rhsTieKey
    ) const;

    /**
     * @brief Returns a stable permutation of @p logRows.
     *
     * Delegates to `SortPermutationByColumn`.
     *
     * @param table Table that owns the rows.
     * @param logRows Log-row indices to order.
     * @return Indices into @p logRows in display order.
     */
    [[nodiscard]] std::vector<std::size_t> Permute(const LogTable &table, std::span<const std::size_t> logRows) const;

private:
    std::size_t mColumnIndex = 0;
    SortDirection mDirection = SortDirection::Ascending;
    const EnumDictRank *mRankForEnumColumn = nullptr;
};

} // namespace loglib
