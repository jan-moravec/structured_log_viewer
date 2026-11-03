/**
 * @brief Reads one line at a time from a probe buffer.
 *
 * Format detection looks at the first few kilobytes of input. NextProbeLine returns the next line of that buffer
 * without its line ending, and notes whether the line was complete.
 *
 * This is part of the parse module.
 */
#pragma once

#include <cstddef>
#include <string_view>

namespace loglib::internal
{

/** @brief One line taken from a byte buffer for format detection. */
struct ProbeLine
{
    /**
     * @brief The text of the line without a trailing carriage return. It is empty for a bare newline, and the caller
     * decides whether to skip blank lines.
     */
    std::string_view line;

    /** @brief The number of bytes this line used, including its newline if it had one. */
    std::size_t bytesConsumed = 0;

    /** @brief The position of the first byte after this line, where the next call starts. */
    std::size_t nextOffset = 0;

    /**
     * @brief True if the line ended with a newline. A last line without one is still returned, because the buffer may
     * be cut off by the probe budget. The caller decides whether such a partial line is useful.
     */
    bool terminated = false;
};

/**
 * @brief Returns the line that starts at offset, without a trailing carriage return. At the end of the buffer it
 * returns an empty line with nextOffset set to the size of the buffer.
 */
[[nodiscard]] inline ProbeLine NextProbeLine(std::string_view bytes, std::size_t offset)
{
    ProbeLine result;
    if (offset >= bytes.size())
    {
        result.nextOffset = bytes.size();
        return result;
    }
    const std::size_t nlPos = bytes.find('\n', offset);
    if (nlPos == std::string_view::npos)
    {
        std::string_view raw = bytes.substr(offset);
        if (!raw.empty() && raw.back() == '\r')
        {
            raw.remove_suffix(1);
        }
        result.line = raw;
        result.bytesConsumed = bytes.size() - offset;
        result.nextOffset = bytes.size();
        result.terminated = false;
        return result;
    }
    std::string_view raw = bytes.substr(offset, nlPos - offset);
    const std::size_t consumedWithNl = (nlPos - offset) + 1;
    if (!raw.empty() && raw.back() == '\r')
    {
        raw.remove_suffix(1);
    }
    result.line = raw;
    result.bytesConsumed = consumedWithNl;
    result.nextOffset = nlPos + 1;
    result.terminated = true;
    return result;
}

} // namespace loglib::internal
