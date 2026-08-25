#pragma once

#include "loglib/filter_expression.hpp"

namespace loglib
{

/**
 * @brief Persisted sort. `columnIndex == -1` means "no sort applied";
 * positive indices index `LogConfiguration::columns` and are remapped by
 * `LogConfigurationManager::MoveColumn`.
 */
struct Sort
{
    int columnIndex = -1;
    bool descending = false;
};

/**
 * @brief View/session state: the filter tree and persisted sort.
 *
 * `LogConfiguration` stores these as adjacent root members so JSON
 * stays flat (`expression`, `sort`). Callers that want the pair as
 * one value construct a `SessionView`.
 */
struct SessionView
{
    FilterExpression expression;
    Sort sort;
};

} // namespace loglib
