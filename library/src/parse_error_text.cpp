#include "loglib/parse_error_text.hpp"

#include <fmt/format.h>

#include <charconv>
#include <optional>
#include <system_error>

namespace loglib
{

namespace
{

constexpr std::string_view SINGLE_PREFIX = "Error on line ";
constexpr std::string_view RANGE_PREFIX = "Error on lines ";
constexpr std::string_view BODY_SEPARATOR = ": ";

/** @brief Consumes a positive decimal number from the front of @p text. */
[[nodiscard]] std::optional<std::uint64_t> TakeNumber(std::string_view &text) noexcept
{
    std::uint64_t value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end == text.data() || value == 0)
    {
        return std::nullopt;
    }
    text.remove_prefix(static_cast<std::size_t>(end - text.data()));
    return value;
}

} // namespace

std::string FormatLineError(std::uint64_t line, std::string_view message)
{
    return fmt::format("{}{}{}{}", SINGLE_PREFIX, line, BODY_SEPARATOR, message);
}

std::string FormatLineRangeError(std::uint64_t firstLine, std::uint64_t lastLine, std::string_view message)
{
    return fmt::format("{}{}-{}{}{}", RANGE_PREFIX, firstLine, lastLine, BODY_SEPARATOR, message);
}

ParseErrorLocation LocateParseError(std::string_view text) noexcept
{
    const ParseErrorLocation unlocated{.message = text};
    std::string_view rest = text;
    std::uint64_t first = 0;
    std::uint64_t last = 0;
    if (rest.starts_with(RANGE_PREFIX))
    {
        rest.remove_prefix(RANGE_PREFIX.size());
        const auto begin = TakeNumber(rest);
        if (!begin || !rest.starts_with('-'))
        {
            return unlocated;
        }
        rest.remove_prefix(1);
        const auto end = TakeNumber(rest);
        if (!end || *end < *begin)
        {
            return unlocated;
        }
        first = *begin;
        last = *end;
    }
    else if (rest.starts_with(SINGLE_PREFIX))
    {
        rest.remove_prefix(SINGLE_PREFIX.size());
        const auto line = TakeNumber(rest);
        if (!line)
        {
            return unlocated;
        }
        first = *line;
        last = *line;
    }
    else
    {
        return unlocated;
    }

    if (!rest.starts_with(':'))
    {
        return unlocated;
    }
    rest.remove_prefix(1);
    while (rest.starts_with(' '))
    {
        rest.remove_prefix(1);
    }
    return ParseErrorLocation{.firstLine = first, .lastLine = last, .message = rest};
}

} // namespace loglib
