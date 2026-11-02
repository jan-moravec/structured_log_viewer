#pragma once

#include "loglib/key_index.hpp"
#include "loglib/time_zone_context.hpp"

#include <chrono>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>

namespace loglib
{

/**
 * @brief Classification of a `parseFormats` string. `Generic` falls through to
 * `date::parse`; the others dispatch to a hand-rolled fast path.
 */
enum class TimestampFormatKind : std::uint8_t
{
    Generic,
    Iso8601_T,
    Iso8601_Space,
    /**
     * @brief RFC 3164 header: `Mmm  D HH:MM:SS` (space- or zero-padded day,
     * English month abbreviation, no year, no timezone). The RFC pins the
     * English abbreviations, so a manual fast path is preferable
     * to `date::from_stream` -- the latter goes through `%b` /
     * `strftime` and depends on the process locale (breaks on
     * non-`C` hosts). The parser injects the year via the standard
     * "if parsed month > current month, use previous year" rollover
     * heuristic since RFC 3164 doesn't carry a year field.
     */
    SyslogRfc3164NoYear,
    /** @brief `Iso8601_T` followed by a required numeric offset. */
    Iso8601TOffset,
    /** @brief `Iso8601_Space` followed by a required numeric offset. */
    Iso8601SpaceOffset,
};

/**
 * @brief Returns the fast-path kind for @p format (`"%FT%T"` / `"%F %T"` /
 * `"%FT%T%Ez"` / `"%F %T%Ez"` / `"%b %e %H:%M:%S"` / `"%b %d %H:%M:%S"`),
 * else `Generic`.
 */
TimestampFormatKind ClassifyTimestampFormat(std::string_view format);

/**
 * @brief Per-line carry-over for the "remember the last successful (keyId, format)"
 * fast path. `kind` caches `ClassifyTimestampFormat(format)`.
 */
struct LastValidTimestampParse
{
    KeyId keyId = INVALID_KEY_ID;
    std::string format;
    TimestampFormatKind kind = TimestampFormatKind::Generic;
};

/** @brief Reusable scratch for the generic `date::parse` fallback. */
struct TimestampParseScratch
{
    std::string str;
    std::istringstream stream;
};

/**
 * @brief ISO-8601 fast path. Accepts `YYYY-MM-DD<sep>HH:MM:SS[.fraction][Z]`;
 * @p dateTimeSep is `'T'` or `' '`. Fractional digits past microseconds are
 * truncated.
 */
bool TryParseIsoTimestamp(std::string_view sv, char dateTimeSep, TimeStamp &out);

/**
 * @brief ISO-8601 fast path for a timestamp with a numeric offset.
 *
 * Accepts `YYYY-MM-DD<sep>HH:MM:SS[.fraction]` followed by `+h`, `+hh`,
 * `+hh:mm`, or `+hhmm` (or `-`). The result is UTC.
 */
bool TryParseIsoOffsetTimestamp(std::string_view sv, char dateTimeSep, TimeStamp &out);

/**
 * @brief RFC 3164 header fast path. Accepts `Mmm d HH:MM:SS` and
 * `Mmm  D HH:MM:SS` (either zero-padded `%d` or space-padded `%e`
 * day). English month abbreviations only (`Jan` -- `Dec`) because
 * RFC 3164 §4.1.2 mandates them; a `date::from_stream("%b ...")`
 * path would depend on the process locale and break on non-`C`
 * hosts. Year is injected via the standard "if parsed month is
 * later than current month, roll back one year" heuristic since
 * the RFC 3164 header omits a year field. @p referenceNow supplies
 * that "current" year and month; it is not cached process-wide.
 */
bool TryParseSyslogRfc3164Timestamp(
    std::string_view sv, TimeStamp &out, std::chrono::system_clock::time_point referenceNow
);

/** @brief RFC 3164 fast path using `system_clock::now()` as the reference. */
bool TryParseSyslogRfc3164Timestamp(std::string_view sv, TimeStamp &out);

/**
 * @brief Slow-path `date::parse` fallback; reuses @p scratch across calls.
 * Fails unless @p format consumes all of @p sv.
 */
bool TryParseGenericTimestamp(
    std::string_view sv, const std::string &format, TimestampParseScratch &scratch, TimeStamp &out
);

/** @brief Picks the fast or slow path based on @p kind. */
bool TryParseTimestamp(
    std::string_view sv,
    const std::string &format,
    TimestampFormatKind kind,
    TimestampParseScratch &scratch,
    TimeStamp &out
);

} // namespace loglib
