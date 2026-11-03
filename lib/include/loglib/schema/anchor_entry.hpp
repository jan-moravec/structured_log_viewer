/**
 * @brief A saved bookmark on one log line.
 *
 * An anchor is identified by the source it belongs to and the line number the parser gave the line. It carries a color
 * index into the theme's anchor palette and an optional one-line note. Anchors are stored in the configuration so they
 * survive reopening the log.
 *
 * This is part of the schema module.
 */
#pragma once

#include <cstdint>
#include <string>

namespace loglib
{

/**
 * @brief A saved bookmark on one log line.
 *
 * A line is identified by the locator of its source together with the line id the parser gave it.
 */
struct AnchorEntry
{
    /** @brief Tells the source files of a session apart. Empty for a session with one file or a network source. */
    std::string locator;

    /** @brief The parser's line id, which grows by one for each line of a source. */
    uint64_t lineId = 0;

    /** @brief The index into the theme's anchor colors. A saved value outside the palette is dropped on load. */
    uint8_t colorIndex = 0;

    /**
     * @brief A short note for the user, empty if there is none.
     *
     * An anchor saved without a note loads with an empty one. A note with several lines is accepted from a file, but
     * editors keep it to one line.
     */
    std::string note;

    friend bool operator==(const AnchorEntry &, const AnchorEntry &) = default;
};

} // namespace loglib
