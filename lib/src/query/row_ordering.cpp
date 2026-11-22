#include "loglib/query/row_ordering.hpp"

#include "loglib/config/log_configuration.hpp"
#include "loglib/core/enum_dictionary.hpp"
#include "loglib/query/internal/row_compare.hpp"
#include "loglib/table/log_table.hpp"

#include <memory>

namespace loglib
{

namespace
{

/**
 * The buffers for comparing single rows. One per thread, and only Compare and LessThan reach them. They do not call
 * each other through code of the caller, and CompareRows does not call back, so two uses never overlap on a thread.
 */
internal::CompareScratch &ThreadScratch()
{
    thread_local internal::CompareScratch scratch;
    return scratch;
}

} // namespace

RowOrdering::RowOrdering(const LogTable &table, std::size_t column, SortDirection direction)
    : mTable(&table), mColumn(column), mDirection(direction)
{
    const auto &columns = table.Layout().Columns();
    if (column >= columns.size() || columns[column].type != ColumnType::Enumeration)
    {
        return;
    }
    const EnumDictionary *dictionary = table.ResolveEnumColumn(column).dictionary;
    if (dictionary != nullptr)
    {
        mRank = std::make_shared<const internal::EnumSortRank>(*dictionary);
        mRankSource = dictionary;
        mRankSize = dictionary->Size();
    }
}

bool RowOrdering::IsOutdated() const
{
    const auto &columns = mTable->Layout().Columns();
    if (mColumn >= columns.size() || columns[mColumn].type != ColumnType::Enumeration)
    {
        // Ranks were built for a column that is no longer an enumeration.
        return mRank != nullptr;
    }
    const EnumDictionary *dictionary = mTable->ResolveEnumColumn(mColumn).dictionary;
    return dictionary != mRankSource || (dictionary != nullptr && dictionary->Size() > mRankSize);
}

int RowOrdering::Compare(std::size_t lhsRow, std::size_t rhsRow) const
{
    return internal::CompareRows(*mTable, lhsRow, rhsRow, mColumn, mRank.get(), ThreadScratch());
}

bool RowOrdering::LessThan(std::size_t lhsRow, std::size_t rhsRow, std::size_t lhsTieKey, std::size_t rhsTieKey) const
{
    const int cmp = Compare(lhsRow, rhsRow);
    if (cmp == 0)
    {
        return lhsTieKey < rhsTieKey;
    }
    return mDirection == SortDirection::Ascending ? cmp < 0 : cmp > 0;
}

std::vector<std::size_t> RowOrdering::Permute(std::span<const std::size_t> rows) const
{
    return internal::SortPermutationByColumn(*mTable, rows, mColumn, mDirection, mRank.get());
}

std::vector<std::size_t> RowOrdering::PermuteWindow(
    std::span<const std::size_t> rows, std::size_t begin, std::size_t end
) const
{
    return internal::SortPermutationWindowByColumn(*mTable, rows, mColumn, mDirection, begin, end, mRank.get());
}

} // namespace loglib
