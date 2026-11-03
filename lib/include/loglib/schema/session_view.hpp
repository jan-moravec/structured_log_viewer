/**
 * @brief Saved sort and filter state of a session.
 *
 * SessionView holds the filter expression and the sort that were active, so reopening a log can restore what the user
 * was looking at. Sort refers to a column by its position in the configuration.
 *
 * This is part of the schema module.
 */
#pragma once

#include "loglib/schema/filter_expression.hpp"

namespace loglib
{

/**
 * @brief The saved sort. A column index of -1 means no sort. Other values index LogConfiguration::columns, and
 * LogConfigurationManager::MoveColumn keeps them up to date.
 */
struct Sort
{
    int columnIndex = -1;
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
