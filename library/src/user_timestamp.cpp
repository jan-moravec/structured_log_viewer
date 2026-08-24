#include "loglib/user_timestamp.hpp"

#include "loglib/log_processing.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace loglib
{

namespace
{

[[nodiscard]] constexpr bool IsAsciiWhitespace(char c) noexcept
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

[[nodiscard]] std::string_view TrimAscii(std::string_view text) noexcept
{
    std::size_t begin = 0;
    while (begin < text.size() && IsAsciiWhitespace(text[begin]))
    {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin && IsAsciiWhitespace(text[end - 1]))
    {
        --end;
    }
    return text.substr(begin, end - begin);
}

[[nodiscard]] constexpr char AsciiToLower(char c) noexcept
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] std::optional<UserTimestampParse> TryParseRelativeShortcut(
    std::string_view text, std::chrono::system_clock::time_point now
)
{
    std::size_t i = 0;
    if (i < text.size() && (text[i] == '+' || text[i] == '-'))
    {
        ++i;
    }
    while (i < text.size() && IsAsciiWhitespace(text[i]))
    {
        ++i;
    }
    if (i >= text.size() || text[i] < '0' || text[i] > '9')
    {
        return std::nullopt;
    }
    const std::size_t digitBegin = i;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9')
    {
        ++i;
    }
    const std::string_view digits = text.substr(digitBegin, i - digitBegin);
    while (i < text.size() && IsAsciiWhitespace(text[i]))
    {
        ++i;
    }
    if (i >= text.size())
    {
        return std::nullopt;
    }
    const char unit = AsciiToLower(text[i]);
    if (unit != 'h' && unit != 'm')
    {
        return std::nullopt;
    }
    ++i;
    while (i < text.size() && IsAsciiWhitespace(text[i]))
    {
        ++i;
    }
    if (i != text.size())
    {
        return std::nullopt;
    }

    std::uint64_t n = 0;
    const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), n);
    if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size())
    {
        return std::nullopt;
    }

    constexpr std::int64_t MICROS_PER_HOUR = 3'600LL * 1'000'000LL;
    constexpr std::int64_t MICROS_PER_MINUTE = 60LL * 1'000'000LL;
    const std::int64_t microsPerUnit = (unit == 'h') ? MICROS_PER_HOUR : MICROS_PER_MINUTE;
    const auto maxN = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() / microsPerUnit);
    if (n > maxN)
    {
        return std::nullopt;
    }

    const std::int64_t offsetMicros = static_cast<std::int64_t>(n) * microsPerUnit;
    const auto nowMicros =
        std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
    return UserTimestampParse{.micros = nowMicros - offsetMicros, .isNaive = false};
}

} // namespace

bool FormatHasZoneSpecifier(std::string_view format) noexcept
{
    for (std::size_t i = 0; i < format.size(); ++i)
    {
        if (format[i] != '%')
        {
            continue;
        }
        const std::size_t next = i + 1;
        if (next >= format.size())
        {
            break;
        }
        if (format[next] == '%')
        {
            ++i;
            continue;
        }
        std::size_t specifier = next;
        if ((format[specifier] == 'E' || format[specifier] == 'O') && specifier + 1 < format.size())
        {
            ++specifier;
        }
        if (format[specifier] == 'z' || format[specifier] == 'Z')
        {
            return true;
        }
    }
    return false;
}

std::optional<UserTimestampParse> ParseUserTimestamp(
    std::string_view input,
    std::span<const std::string> columnParseFormats,
    std::chrono::system_clock::time_point now
)
{
    const std::string_view trimmed = TrimAscii(input);
    if (trimmed.empty())
    {
        return std::nullopt;
    }

    if (const auto relative = TryParseRelativeShortcut(trimmed, now); relative.has_value())
    {
        return relative;
    }

    std::vector<std::string> candidates;
    candidates.reserve(columnParseFormats.size() + 2);
    for (const auto &fmt : columnParseFormats)
    {
        candidates.push_back(fmt);
    }
    for (const char *fallback : {"%FT%T", "%F %T"})
    {
        if (std::find(candidates.begin(), candidates.end(), fallback) == candidates.end())
        {
            candidates.emplace_back(fallback);
        }
    }

    TimestampParseScratch scratch;
    for (const auto &fmt : candidates)
    {
        TimeStamp parsed{};
        if (TryParseTimestamp(trimmed, fmt, ClassifyTimestampFormat(fmt), scratch, parsed))
        {
            return UserTimestampParse{
                .micros = parsed.time_since_epoch().count(), .isNaive = !FormatHasZoneSpecifier(fmt)
            };
        }
    }
    return std::nullopt;
}

} // namespace loglib
