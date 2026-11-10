/**
 * @brief Parses timestamps typed by the user.
 *
 * The go-to-timestamp box accepts the configured formats, relative shortcuts and ISO text. The result says whether the
 * input named a time zone. If it did not, the caller must shift it through the display zone before comparing it with
 * stored UTC timestamps.
 *
 * This is part of the time module.
 */
#pragma once

#include "loglib/time/timestamp_parsing.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace loglib
{

/**
 * @brief A time the user typed, as UTC microseconds since the epoch.
 *
 * isNaive is true when the text names no time zone, that is, the format has no zone and the text does not end in Z. The
 * caller must then read the time as local time in the display zone before comparing it with stored UTC times. Relative
 * times such as 5m are never naive.
 */
struct UserTimestampParse
{
    std::int64_t micros = 0;
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

/** @brief Like the overload above, for a prepared format, which knows whether it has a zone without scanning. */
[[nodiscard]] bool IsNaiveTimestamp(std::string_view text, const TimestampFormat &format) noexcept;

/**
 * @brief Parses a time the user typed, either absolute or relative.
 *
 * It tries a relative form first: an optional sign, a number, and h or m, in any case and with spaces allowed. Any sign
 * means that many hours or minutes before now. Then it tries each of the column's formats, then %FT%T and %F %T. Spaces
 * at both ends of the input are ignored. It does not convert a naive time to UTC, so the caller does that with
 * LocalMicrosecondsSinceEpochToUtc.
 *
 * @param now The reference time for relative input.
 * @return The parsed time, or nullopt if the text does not parse or the value overflows.
 */
[[nodiscard]] std::optional<UserTimestampParse> ParseUserTimestamp(
    std::string_view input, std::span<const std::string> columnParseFormats, std::chrono::system_clock::time_point now
);

} // namespace loglib
