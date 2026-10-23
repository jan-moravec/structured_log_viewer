#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace loglib
{

/**
 * @brief Physical line span and body of one parse-error string.
 *
 * Parsers report errors as display strings. `FormatLineError` and
 * `FormatLineRangeError` write the location prefix; `LocateParseError`
 * reads it back. `firstLine` and `lastLine` are 1-based and both 0 when
 * the error has no location. `message` views the text after the prefix,
 * or the whole input when there is no prefix.
 */
struct ParseErrorLocation
{
    std::uint64_t firstLine = 0;
    std::uint64_t lastLine = 0;
    std::string_view message;
};

/**
 * @brief Formats an error located on one physical line.
 *
 * @param line 1-based line number.
 * @param message Error body.
 * @return `Error on line <line>: <message>`.
 */
[[nodiscard]] std::string FormatLineError(std::uint64_t line, std::string_view message);

/**
 * @brief Formats an error that spans several physical lines.
 *
 * @param firstLine 1-based first line.
 * @param lastLine 1-based last line.
 * @param message Error body.
 * @return `Error on lines <firstLine>-<lastLine>: <message>`.
 */
[[nodiscard]] std::string FormatLineRangeError(
    std::uint64_t firstLine, std::uint64_t lastLine, std::string_view message
);

/**
 * @brief Reads the location prefix that the format functions write.
 *
 * @param text Parse-error string. The result views into it.
 * @return The line span and body, or a zero span and the whole text.
 */
[[nodiscard]] ParseErrorLocation LocateParseError(std::string_view text) noexcept;

} // namespace loglib
