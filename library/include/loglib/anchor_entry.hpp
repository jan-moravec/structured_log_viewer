#pragma once

#include <cstdint>
#include <string>

namespace loglib
{

/**
 * @brief A persisted bookmark on one log line.
 *
 * Identified by `(locator, lineId)`: `lineId` is the parser's
 * monotonic per-`LineSource` id; `locator` disambiguates files
 * in a multi-file session (empty for single-file / network).
 * `colorIndex` indexes `Theme::anchorPalette`; out-of-range
 * values are dropped on load.
 *
 * `note` is a one-line free-form annotation surfaced in the
 * Anchors dock, row tooltips, and the record-detail view.
 * Empty for anchors saved by older builds -- Glaze's
 * `error_on_unknown_keys=false` + default-on-missing lets
 * them load cleanly. Multi-line values are legal on disk but
 * the editors sanitise on commit, so the on-disk shape is a
 * single line by construction.
 */
struct AnchorEntry
{
    std::string locator;
    uint64_t lineId = 0;
    uint8_t colorIndex = 0;
    std::string note;

    friend bool operator==(const AnchorEntry &, const AnchorEntry &) = default;
};

} // namespace loglib
