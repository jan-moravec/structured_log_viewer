/**
 * @brief A saved rule that colors matching rows.
 *
 * A HighlightRule tests rows with a FilterCondition, the same condition a filter uses, and when it matches it applies a
 * HighlightStyle. Rules belong to the configuration, so they follow the column layout across sessions, while filters
 * belong to one session.
 *
 * This is part of the config module.
 */
#pragma once

#include "loglib/config/filter_expression.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace loglib
{

/**
 * @brief The number of highlight color pairs a theme offers. A style refers to them by number starting at 1, and 0
 * means no color is chosen.
 */
inline constexpr std::size_t HIGHLIGHT_PALETTE_SIZE = 16;

/** @brief How a highlight rule changes the rows it matches. */
struct HighlightStyle
{
    /** @brief The theme color for text, from 1 to HIGHLIGHT_PALETTE_SIZE. Zero keeps the normal color. */
    uint8_t foregroundIndex = 0;

    /** @brief The theme color for the background, with the same values as foregroundIndex. */
    uint8_t backgroundIndex = 0;

    bool bold = false;
    bool italic = false;

    friend bool operator==(const HighlightStyle &, const HighlightStyle &) = default;
};

/**
 * @brief A rule that changes how matching rows look.
 *
 * A rule belongs to the configuration, so it follows the column layout from session to session, for example a rule on
 * service equal to auth. Filters belong to one session instead.
 *
 * The condition finds its column by its column keys, so it keeps working when columns are moved or added. A rule whose
 * column cannot be found does nothing. Rules are applied in list order, and the last matching rule wins for a row.
 */
struct HighlightRule
{
    /** @brief A label for the user. */
    std::string name;

    /**
     * @brief A disabled rule stays in the list but does not change any row, so it can be paused without deleting it.
     */
    bool enabled = true;

    /** @brief Which rows the rule matches. */
    FilterCondition condition;

    /** @brief What the rule does to the rows it matches. */
    HighlightStyle style;

    friend bool operator==(const HighlightRule &, const HighlightRule &) = default;
};

} // namespace loglib
