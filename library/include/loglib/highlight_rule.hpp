#pragma once

#include "loglib/filter_expression.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace loglib
{

/**
 * @brief A user-defined row-highlighting rule.
 *
 * Configuration-scope: persisted alongside `columns` so a
 * rule bound to `service == "auth"` roams with the column
 * schema. Filters, by contrast, are session-scope.
 *
 * Bound by `columnKeys` (matching `Column::keys`) so rules
 * survive `MoveColumn`, cross-source apply, and unrelated
 * column additions. Unresolvable rules are inert at match
 * time (editor greys them out).
 *
 * The `type` / `matchType` / `filter*` fields mirror `LeafRule`
 * so the shared `CompileLeaf` step applies uniformly to
 * filters and highlight rules (the app-side compile builds a
 * `LeafRule` shim from `HighlightRule` and passes it to the
 * same factory). Rules apply in vector order, last match wins
 * per row.
 */
struct HighlightRule
{
    /**
     * @brief Aliased so filter-side and highlight-side rules share a
     * single canonical `Type` / `Match` enum without changing
     * the on-disk JSON.
     */
    using Type = loglib::LeafRule::Type;
    using Match = loglib::LeafRule::Match;

    /** @brief User-visible label. Free-form. */
    std::string name;

    /**
     * @brief Disabled rules stay in the list but don't paint, so a
     * triage playbook can pause a rule without deleting it.
     */
    bool enabled = true;

    /**
     * @brief Column identity (subset-matched against `Column::keys`).
     * Empty = no column bound (rule is inert). Usually a
     * single-entry vector.
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

    /** @brief 0 = inherit; 1..`HIGHLIGHT_PALETTE_SIZE` = theme slot. */
    uint8_t foregroundIndex = 0;
    uint8_t backgroundIndex = 0;
    bool bold = false;
    bool italic = false;

    friend bool operator==(const HighlightRule &, const HighlightRule &) = default;
};

} // namespace loglib
