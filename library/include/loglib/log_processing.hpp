#pragma once

#include "key_index.hpp"
#include "log_configuration.hpp"
#include "log_data.hpp"
#include "log_line.hpp"
#include "time_zone_context.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
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
};

/**
 * @brief Returns the fast-path kind for @p format (`"%FT%T"` / `"%F %T"` /
 * `"%b %e %H:%M:%S"` / `"%b %d %H:%M:%S"`), else `Generic`.
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
 * @brief ISO-8601 fast path. Accepts `YYYY-MM-DD<sep>HH:MM:SS[.fff[fff]]` with up to
 * six fractional digits; @p dateTimeSep is `'T'` or `' '`. An epoch-zero
 * result is reported as a failure.
 */
bool TryParseIsoTimestamp(std::string_view sv, char dateTimeSep, TimeStamp &out);

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

/** @brief Slow-path `date::parse` fallback; reuses @p scratch across calls. */
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

/** @brief Promotes timestamp columns in @p logData; returns per-line failure messages. */
std::vector<std::string> ParseTimestamps(LogData &logData, const LogConfiguration &configuration);

/**
 * @brief Promotes one configured `Type::Time` column over @p lines in place.
 * Caller must ensure `column.type == Type::Time`. Pass a sub-span to
 * restrict the back-fill to a slice of a larger vector (e.g. only the rows
 * just appended in a streaming batch). Returns per-line failure messages.
 */
std::vector<std::string> BackfillTimestampColumn(const Column &column, std::span<LogLine> lines);

/**
 * @brief Tag selecting the `void` overload that skips per-line "Failed to parse"
 * formatting on the streaming hot path.
 */
enum class BackfillErrors : uint8_t
{
    Discard
};

/** @brief `void` overload of `BackfillTimestampColumn` that drops error messages. */
void BackfillTimestampColumn(const Column &column, std::span<LogLine> lines, BackfillErrors discardErrors);

int64_t TimeStampToLocalMillisecondsSinceEpoch(TimeStamp timeStamp, const TimeZoneContext &timeZone);
int64_t TimeStampToLocalMillisecondsSinceEpoch(TimeStamp timeStamp);

int64_t UtcMicrosecondsToLocalMilliseconds(int64_t microseconds, const TimeZoneContext &timeZone);
int64_t UtcMicrosecondsToLocalMilliseconds(int64_t microseconds);

TimeStamp LocalMillisecondsSinceEpochToTimeStamp(int64_t milliseconds, const TimeZoneContext &timeZone);
TimeStamp LocalMillisecondsSinceEpochToTimeStamp(int64_t milliseconds);

/**
 * @brief Convert @p localMicroseconds -- interpreted as a wall-clock
 * instant in @p timeZone -- to UTC epoch microseconds.
 *
 * DST edge cases resolve via earliest-instant choice:
 *   * Ambiguous "fall-back" hour: the earlier candidate.
 *   * Non-existent "spring-forward" gap: the transition boundary
 *     (the first real instant after the gap).
 * Non-DST exceptions (far-future dates past the tzdata table,
 * corrupt zone entries) yield the naive value so the Goto Timestamp
 * slot stays exception-safe. UTC contexts pass the value through.
 */
int64_t LocalMicrosecondsSinceEpochToUtc(int64_t localMicroseconds, const TimeZoneContext &timeZone);

/**
 * @brief Convenience overload using `ProcessDefaultTimeZone()`.
 *
 * That default is process-wide convenience, not a prerequisite for
 * the explicit-context overload.
 */
int64_t LocalMicrosecondsSinceEpochToUtc(int64_t localMicroseconds);

/** @brief Formats UTC microseconds since epoch as a `%F %T`-style local-time string. */
std::string UtcMicrosecondsToDateTimeString(int64_t microseconds, const TimeZoneContext &timeZone);
std::string UtcMicrosecondsToDateTimeString(int64_t microseconds);

/** @brief Formats a `TimeStamp` as a `%F %T`-style local-time string. */
std::string TimeStampToDateTimeString(TimeStamp timeStamp, const TimeZoneContext &timeZone);
std::string TimeStampToDateTimeString(TimeStamp timeStamp);

} // namespace loglib
