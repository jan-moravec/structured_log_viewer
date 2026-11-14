/**
 * @brief Saved sort and filter state of a session.
 *
 * SessionView holds the filter expression and the sort that were active, so reopening a log can restore what the user
 * was looking at. Sort refers to a column by its keys, so moving a column does not change it.
 *
 * This is part of the schema module.
 */
#pragma once

#include "loglib/schema/filter_expression.hpp"

#include <string>
#include <vector>

namespace loglib
{

/**
 * @brief The saved sort. The column is named by its keys, found the way filter leaves find theirs, so a column move
 * needs no update. Empty keys mean no sort. Use ResolveLeafColumnByKeys to get the column index.
 */
struct Sort
{
    /** @brief The keys of the sorted column. Empty means no sort. */
    std::vector<std::string> columnKeys;

    /** @brief True to sort from the largest value to the smallest. */
    bool descending = false;
};

/**
 * @brief The filter and the sort of a session as one value.
 *
 * LogConfiguration stores the two as separate members to keep its JSON flat. Build a SessionView when a caller wants to
 * handle them together.
 */
struct SessionView
{
    FilterExpression expression;
    Sort sort;
};

} // namespace loglib
