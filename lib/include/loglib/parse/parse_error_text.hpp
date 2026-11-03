/**
 * @brief Writes and reads the line prefix of parse error messages.
 *
 * Parsers report problems as text. These helpers add a line or line range prefix to a message and read it back, so the
 * interface can jump to the line an error refers to.
 *
 * This is part of the parse module.
 */
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace loglib
{

/**
 * @brief The lines an error refers to, and the text of the error without its location.
 *
 * Parsers report errors as text with a location prefix. FormatLineError and FormatLineRangeError write the prefix and
 * LocateParseError reads it back. Line numbers start at 1, and both are 0 when the error has no location. The message
 * is a view of the text after the prefix, or of the whole text when there is no prefix.
 */
struct ParseErrorLocation
{
    std::uint64_t firstLine = 0;
    std::uint64_t lastLine = 0;
    std::string_view message;
};

/** @brief Formats an error on one line, as "Error on line N: message". Line numbers start at 1. */
[[nodiscard]] std::string FormatLineError(std::uint64_t line, std::string_view message);

/** @brief Formats an error that spans several lines, as "Error on lines N-M: message". Line numbers start at 1. */
[[nodiscard]] std::string FormatLineRangeError(
    std::uint64_t firstLine, std::uint64_t lastLine, std::string_view message
);

/**
 * @brief Reads the location back from an error text. Without a prefix the lines are 0 and the message is the whole
 * text. The result views into text.
 */
[[nodiscard]] ParseErrorLocation LocateParseError(std::string_view text) noexcept;

} // namespace loglib
