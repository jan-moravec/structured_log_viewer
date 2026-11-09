/**
 * @brief Reads timestamps from text.
 *
 * Most logs use a few well-known timestamp formats, so ISO 8601 and syslog headers have hand-written fast paths. Any
 * other format falls back to a general parser. ClassifyTimestampFormat decides which path a format string takes, and
 * TryParseTimestamp runs it. A scratch object lets callers reuse buffers across many lines.
 *
 * This is part of the time module.
 */
#pragma once

#include "loglib/core/key_index.hpp"
#include "loglib/time/time_zone_context.hpp"

#include <chrono>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>

namespace loglib
{

/**
 * @brief Says how a time format is parsed: by one of the fast hand-written parsers, or by the general parser.
 *
 * Generic goes through date::parse. The other kinds have a hand-written parser that is much faster.
 */
enum class TimestampFormatKind : std::uint8_t
{
    Generic,
    Iso8601T,
    Iso8601Space,
    /**
     * @brief The RFC 3164 syslog header: month abbreviation, day with a space or zero in front, and time, with no year
     * and no zone.
     *
     * The RFC fixes the English month names, so a hand-written parser is used. date::from_stream would depend on the
     * process locale for %b and fail on hosts that are not set to the C locale. The year is filled in: a month later
     * than the current month is taken to be in the previous year.
     */
    SyslogRfc3164NoYear,
    /** @brief Like Iso8601T, followed by a required numeric offset. */
    Iso8601TOffset,
    /** @brief Like Iso8601Space, followed by a required numeric offset. */
    Iso8601SpaceOffset,
};

/**
 * @brief Chooses the parser for a time format.
 *
 * The formats %FT%T, %F %T, %FT%T%Ez, %F %T%Ez, %b %e %H:%M:%S and %b %d %H:%M:%S have fast parsers. Any other format
 * gives Generic.
 */
TimestampFormatKind ClassifyTimestampFormat(std::string_view format);

/**
 * @brief Remembers the key and format that parsed the last timestamp, so the next line tries them first. The kind is
 * the result of ClassifyTimestampFormat for the format.
 */
struct LastValidTimestampParse
{
    KeyId keyId = INVALID_KEY_ID;
    std::string format;
    TimestampFormatKind kind = TimestampFormatKind::Generic;
};

/** @brief Buffers for the general parser, kept between calls to avoid allocating each time. */
struct TimestampParseScratch
{
    std::string str;
    std::istringstream stream;
};

/**
 * @brief Parses a time like 2024-05-01T12:30:00.123456Z. The date and time are separated by dateTimeSep, which is T or
 * a space.
 *
 * The fraction and the Z are optional, and digits beyond microseconds are cut off.
 */
bool TryParseIsoTimestamp(std::string_view sv, char dateTimeSep, TimeStamp &out);

/**
 * @brief Parses a time like the previous function, followed by an offset written as +h, +hh, +hh:mm or +hhmm, or with a
 * minus sign. The result is in UTC.
 */
bool TryParseIsoOffsetTimestamp(std::string_view sv, char dateTimeSep, TimeStamp &out);

/**
 * @brief Parses the RFC 3164 syslog header, such as Mmm d HH:MM:SS or Mmm  D HH:MM:SS, with a zero or space in front of
 * a one-digit day.
 *
 * Only the English month names are accepted, as the RFC requires. The header has no year, so the year comes from
 * referenceNow: a month later than its month is taken to be in the previous year. referenceNow is not remembered
 * between calls.
 */
bool TryParseSyslogRfc3164Timestamp(
    std::string_view sv, TimeStamp &out, std::chrono::system_clock::time_point referenceNow
);

/**
 * @brief Parses with date::parse and an arbitrary format. It succeeds only if the format uses all of the text. The
 * scratch buffers are reused across calls.
 */
bool TryParseGenericTimestamp(
    std::string_view sv, const std::string &format, TimestampParseScratch &scratch, TimeStamp &out
);

/**
 * @brief Parses a timestamp with the parser that kind selects.
 *
 * referenceTime is the "now" used for formats without a year. A static parse fixes it once, so every line of one file
 * gets the same year. A live stream reads the clock again for each batch.
 */
bool TryParseTimestamp(
    std::string_view sv,
    const std::string &format,
    TimestampFormatKind kind,
    TimestampParseScratch &scratch,
    std::chrono::system_clock::time_point referenceTime,
    TimeStamp &out
);

} // namespace loglib
