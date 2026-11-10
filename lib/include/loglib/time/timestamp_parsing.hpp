/**
 * @brief Reads timestamps from text.
 *
 * A TimestampFormat is a time format prepared once for parsing many values. Most logs use a few well-known formats, so
 * ISO 8601 and syslog headers have hand-written parsers with their own rules. Any other format made only of common
 * fields, such as the Apache access log format, is turned into a list of steps that a hand-written reader runs, with
 * the same results as date::parse. The rest falls back to date::parse itself. A scratch object lets callers reuse
 * buffers across many values.
 *
 * This is part of the time module.
 */
#pragma once

#include "loglib/core/key_index.hpp"
#include "loglib/time/time_zone_context.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace loglib
{

/**
 * @brief Says how a time format is parsed.
 *
 * The ISO and syslog kinds have hand-written parsers with rules of their own. Compiled formats are read by a
 * hand-written reader that gives the same result as date::parse. Generic formats go through date::parse.
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
    /**
     * @brief A format made only of the fields %Y %m %d %e %H %M %S %F %T %b %B %h %a %A %z %Ez %Z and %%, with each
     * field at most once and a year, month and day present. A hand-written reader parses it.
     */
    Compiled,
    /**
     * @brief A format that date::parse would read with an unsafe field width, such as %0S or a width above 9. It never
     * matches.
     */
    Rejected,
};

/** @brief Buffers for the general parser, kept between calls to avoid allocating each time. */
struct TimestampParseScratch
{
    std::string str;
    std::istringstream stream;
};

/**
 * @brief A time format prepared once for parsing many values.
 *
 * Building one picks the parser and, for a Compiled format, turns the format into steps, so a parse does no work on the
 * format text. Copying is cheap enough for configuration code but not meant for a hot path.
 */
class TimestampFormat
{
public:
    /** @brief Prepares format. Any text is accepted; a format that can never match has the kind Rejected. */
    explicit TimestampFormat(std::string format);

    /** @brief The format as written. */
    [[nodiscard]] const std::string &Text() const noexcept;

    [[nodiscard]] TimestampFormatKind Kind() const noexcept;

    /**
     * @brief True if the format reads a zone with %z, %Ez, %Oz or %Z. A value parsed without one is a wall-clock time
     * that still needs a zone.
     */
    [[nodiscard]] bool HasZone() const noexcept;

    /**
     * @brief Parses text, which must match the whole format. Returns false and leaves out unchanged or zero on failure.
     *
     * referenceTime is the "now" used for formats without a year. A static parse fixes it once, so every line of one
     * file gets the same year. A live stream reads the clock again for each batch.
     */
    bool Parse(
        std::string_view text,
        TimestampParseScratch &scratch,
        std::chrono::system_clock::time_point referenceTime,
        TimeStamp &out
    ) const;

    /** @brief One action of a Compiled format. */
    struct Step
    {
        enum class Op : std::uint8_t
        {
            Literal,
            Space,
            Year,
            Month,
            Day,
            Hour,
            Minute,
            Second,
            MonthName,
            WeekdayName,
            Offset,
            OffsetWithColon,
            ZoneName,
        };
        Op op = Op::Literal;
        char literal = 0;
    };

private:
    /** @brief Runs the steps. Returns 1 on success, 0 on failure, and -1 when only date::parse can decide. */
    [[nodiscard]] int RunSteps(std::string_view text, TimeStamp &out) const;

    std::string mText;
    std::vector<Step> mSteps;
    TimestampFormatKind mKind = TimestampFormatKind::Generic;
    bool mHasZone = false;
};

/**
 * @brief Remembers the key and format that parsed the last timestamp, so the next line tries them first. formatIndex
 * is a position in the column's list of formats.
 */
struct LastValidTimestampParse
{
    KeyId keyId = INVALID_KEY_ID;
    std::size_t formatIndex = 0;
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

} // namespace loglib
