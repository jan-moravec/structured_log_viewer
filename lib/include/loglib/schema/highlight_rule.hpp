/**
 * @brief A saved rule that colors matching rows.
 *
 * A HighlightRule matches rows the same way a filter leaf does, but it only changes how rows look. Rules belong to the
 * configuration, so they follow the column layout across sessions, while filters belong to one session.
 *
 * This is part of the schema module.
 */
#pragma once

#include "loglib/schema/filter_expression.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace loglib
{

/**
 * @brief A rule that changes how matching rows look.
 *
 * A rule belongs to the configuration, so it follows the column layout from session to session, for example a rule on
 * service equal to auth. Filters belong to one session instead.
 *
 * A rule finds its column by columnKeys, which are matched against Column::keys, so it keeps working when columns are
 * moved or added. A rule whose column cannot be found does nothing. The type, matchType and filter fields have the same
 * meaning as in LeafRule, so ToLeafRule and CompileLeaf treat filters and highlight rules alike. Rules are applied in
 * list order, and the last matching rule wins for a row.
 */
struct HighlightRule
{
    /**
     * @brief The same types as a filter leaf uses, so both kinds of rule share them and the saved files do not change.
     */
    using Type = loglib::LeafRule::Type;
    using Match = loglib::LeafRule::Match;

    /** @brief A label for the user. */
    std::string name;

    /**
     * @brief A disabled rule stays in the list but does not change any row, so it can be paused without deleting it.
     */
    bool enabled = true;

    /**
     * @brief The keys of the column to test, matched as a subset of Column::keys. Usually one key. With none, the rule
     * does nothing.
     */
    std::vector<std::string> columnKeys;

    Type type = Type::String;
    std::optional<Match> matchType;
    std::optional<std::string> filterString;
    std::optional<int64_t> filterBegin;
    std::optional<int64_t> filterEnd;
    std::optional<double> filterMinValue;
    std::optional<double> filterMaxValue;
    std::vector<std::string> filterValues;

    /** @brief The theme color for text, from 1 to HIGHLIGHT_PALETTE_SIZE. Zero keeps the normal color. */
    uint8_t foregroundIndex = 0;
    /** @brief The theme color for the background, with the same values as foregroundIndex. */
    uint8_t backgroundIndex = 0;
    bool bold = false;
    bool italic = false;

    friend bool operator==(const HighlightRule &, const HighlightRule &) = default;
};

} // namespace loglib
