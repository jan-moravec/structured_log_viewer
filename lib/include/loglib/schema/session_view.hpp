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
 * @brief Persisted sort. columnIndex == -1 means "no sort applied";
 * positive indices index LogConfiguration::columns and are remapped by
 * LogConfigurationManager::MoveColumn.
 */
struct Sort
{
    int columnIndex = -1;
    bool descending = false;
};

/**
 * @brief View/session state: the filter tree and persisted sort.
 *
 * LogConfiguration stores these as adjacent root members so JSON
 * stays flat (expression, sort). Callers that want the pair as
 * one value construct a SessionView.
 */
struct SessionView
{
    FilterExpression expression;
    Sort sort;
};

} // namespace loglib
