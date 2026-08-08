#pragma once

#include <cstddef>
#include <string_view>

namespace loglib::internal
{

/// One probe line extracted from a byte buffer.
struct ProbeLine
{
    /// Contents of the line with any trailing `\r` stripped. Empty
    /// for a bare `\n` in the input (the caller decides whether to
    /// skip blank lines).
    std::string_view line;

    /// Cumulative bytes consumed from the input, including the
    /// terminating `\n` if one was found.
    std::size_t bytesConsumed = 0;

    /// Position of the first byte *after* this line in the input,
    /// used to advance the cursor for the next call.
    std::size_t nextOffset = 0;

    /// True iff the line ended with `\n` in the input. A final
    /// unterminated line (buffer truncated by the probe budget) is
    /// still returned so callers can inspect the partial content;
    /// they must decide whether a partial line is meaningful.
    bool terminated = false;
};

/// Extract the line at @p offset from @p bytes. At end-of-buffer,
/// returns an empty `ProbeLine` with `nextOffset == bytes.size()`.
/// Trailing `\r` is stripped from the returned view.
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
