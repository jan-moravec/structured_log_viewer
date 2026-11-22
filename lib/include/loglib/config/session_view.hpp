/**
 * @brief What a user was looking at in one session: filter, sort, highlight rules and anchors.
 *
 * SessionView collects the parts of a configuration that belong to a session and not to the column layout of a table.
 * A desktop session owns one and slp builds one from its flags and its configuration file. Every part finds its column
 * by keys, so moving a column never changes it.
 *
 * This is part of the config module. LogConfiguration saves a SessionView in a configuration file.
 */
#pragma once

#include "loglib/config/anchor_entry.hpp"
#include "loglib/config/filter_expression.hpp"
#include "loglib/config/highlight_rule.hpp"

#include <string>
#include <vector>

namespace loglib
{

/** @brief The direction of a sort. */
enum class SortDirection
{
    /** @brief From the smallest value to the largest. */
    Ascending,
    /** @brief From the largest value to the smallest. */
    Descending,
};

/**
 * @brief The saved sort. The column is named by its keys, found the way filter conditions find theirs, so a column move
 * needs no update. Empty keys mean no sort. Use FindColumnByKeys to get the column index.
 */
struct Sort
{
    /** @brief The keys of the sorted column. Empty means no sort. */
    std::vector<std::string> columnKeys;

    /** @brief The direction of the sort. */
    SortDirection direction = SortDirection::Ascending;
};

/** @brief The filter, sort, highlight rules and anchors of a session. */
struct SessionView
{
    /**
     * @brief The filter. The default is an empty And, which accepts every row, so a new session shows everything.
     * Leaves find their column by FilterCondition::columnKeys and not by position, so moving a column does not change
     * the filter.
     */
    FilterExpression expression;

    /** @brief The sort. By default there is none. */
    Sort sort;

    /**
     * @brief The highlight rules. Order matters, because the last matching rule wins. Rules are compiled against the
     * table, because a leaf needs the column type and the enumeration dictionary.
     */
    std::vector<HighlightRule> highlightRules;

    /**
     * @brief The anchors. They are written sorted by path key and line id so saved files differ little, and may be in
     * any order when read.
     */
    std::vector<AnchorEntry> anchors;
};

} // namespace loglib
