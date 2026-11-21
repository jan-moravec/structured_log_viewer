/**
 * @brief Parses time text that a person typed.
 *
 * Typed time text can use the configured formats of a column, relative shortcuts such as -5m, or ISO text. The result
 * says whether the text named a time zone. If it did not, the caller must read it in the zone it wants before comparing
 * it with stored UTC timestamps.
 *
 * This is part of the time module.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace loglib
{

/**
 * @brief A time that a person typed, in microseconds since the epoch.
 *
 * isNaive is true when the text names no time zone, that is, the format has no zone and the text does not end in Z. The
 * microseconds are then the wall-clock reading as if it were UTC, and the caller must read it as local time in a zone
 * of its choice, for example with TimeZoneContext::LocalToUtc, before comparing it with stored UTC times. Otherwise the
 * microseconds are UTC. Relative times such as 5m are never naive.
 */
struct TimeInput
{
    std::int64_t microseconds = 0;
    bool isNaive = false;
};

/**
 * @brief True if a date::parse format contains a zone specifier, so a parse with it gives UTC.
 *
 * The specifiers are %z, %Z, %Ez and %Oz. A %% is a literal percent sign. A malformed start such as %E%z still counts
 * for its %z.
 */
[[nodiscard]] bool FormatHasZoneSpecifier(std::string_view format) noexcept;

/** @brief True if neither the format nor a trailing Z in the text that it parsed gives a zone. */
[[nodiscard]] bool IsNaiveTimestamp(std::string_view text, std::string_view format) noexcept;

/**
 * @brief Parses time text that a person typed, either absolute or relative.
 *
 * The word now, in any case, gives the reference time. It tries a relative form next: an optional sign, a number, and
 * h or m, in any case and with spaces allowed. Any sign means that many hours or minutes before now. Then it tries each
 * of the column's formats, then %FT%T and %F %T. Spaces at both ends of the input are ignored. It does not convert a
 * naive time to UTC, so the caller does that with TimeZoneContext::LocalToUtc.
 *
 * @param now The reference time for relative input.
 * @return The parsed time, or nullopt if the text does not parse or the value overflows.
 */
[[nodiscard]] std::optional<TimeInput> ParseTimeInput(
    std::string_view input, std::span<const std::string> columnParseFormats, std::chrono::system_clock::time_point now
);

} // namespace loglib
