#include "loglib/row_ordering.hpp"

#include "loglib/log_table.hpp"

namespace loglib
{

RowOrdering::RowOrdering(
    std::size_t columnIndex, SortDirection direction, const EnumDictRank *rankForEnumColumn
) noexcept
    : mColumnIndex(columnIndex), mDirection(direction), mRankForEnumColumn(rankForEnumColumn)
{
}

int RowOrdering::Compare(const LogTable &table, std::size_t lhsRow, std::size_t rhsRow) const
{
    return CompareRows(table, lhsRow, rhsRow, mColumnIndex, mRankForEnumColumn);
}

bool RowOrdering::LessThan(
    const LogTable &table, std::size_t lhsRow, std::size_t rhsRow, std::size_t lhsTieKey, std::size_t rhsTieKey
) const
{
    const int cmp = Compare(table, lhsRow, rhsRow);
    if (cmp == 0)
    {
        return lhsTieKey < rhsTieKey;
    }
    return Ascending() ? cmp < 0 : cmp > 0;
}

std::vector<std::size_t> RowOrdering::Permute(const LogTable &table, std::span<const std::size_t> logRows) const
{
    return SortPermutationByColumn(table, logRows, mColumnIndex, Ascending(), mRankForEnumColumn);
}

} // namespace loglib
