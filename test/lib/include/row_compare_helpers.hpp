/**
 * @brief Calls the internal row comparison and sorting functions with plain arguments, for tests and benchmarks.
 *
 * The functions in loglib::internal take a sort direction and a scratch for formatting values. Tests of the comparison
 * rules want to say true or false for the direction and to leave the scratch out, so these wrappers do that.
 */
#pragma once

#include <loglib/query/internal/row_compare.hpp>

#include <cstddef>
#include <span>
#include <vector>

namespace row_compare_helpers
{

using loglib::internal::EnumSortRank;

[[nodiscard]] inline int CompareRows(
    const loglib::LogTable &table,
    std::size_t lhsRow,
    std::size_t rhsRow,
    std::size_t columnIndex,
    const EnumSortRank *rank = nullptr
)
{
    thread_local loglib::internal::CompareScratch scratch;
    return loglib::internal::CompareRows(table, lhsRow, rhsRow, columnIndex, rank, scratch);
}

[[nodiscard]] inline loglib::SortDirection DirectionOf(bool ascending) noexcept
{
    return ascending ? loglib::SortDirection::Ascending : loglib::SortDirection::Descending;
}

[[nodiscard]] inline std::vector<std::size_t> SortPermutationByColumn(
    const loglib::LogTable &table,
    std::span<const std::size_t> logRows,
    std::size_t columnIndex,
    bool ascending,
    const EnumSortRank *rank = nullptr
)
{
    return loglib::internal::SortPermutationByColumn(table, logRows, columnIndex, DirectionOf(ascending), rank);
}

[[nodiscard]] inline std::vector<std::size_t> SortPermutationWindowByColumn(
    const loglib::LogTable &table,
    std::span<const std::size_t> logRows,
    std::size_t columnIndex,
    bool ascending,
    std::size_t begin,
    std::size_t end,
    const EnumSortRank *rank = nullptr
)
{
    return loglib::internal::SortPermutationWindowByColumn(
        table, logRows, columnIndex, DirectionOf(ascending), begin, end, rank
    );
}

} // namespace row_compare_helpers
