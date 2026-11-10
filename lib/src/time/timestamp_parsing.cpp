#include "loglib/time/timestamp_parsing.hpp"

#include "loglib/time/user_timestamp.hpp"

#include <date/date.h>
#include <date/tz.h>
#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace loglib
{

namespace
{

constexpr int DECIMAL_RADIX = 10;
constexpr size_t ISO_DASH1_INDEX = 4;
constexpr size_t MONTH_DIGITS_OFFSET = 5;
constexpr size_t ISO_DASH2_INDEX = 7;
constexpr size_t DAY_DIGITS_OFFSET = 8;
constexpr size_t DATE_TIME_SEPARATOR_INDEX = 10;
constexpr size_t HOUR_DIGITS_OFFSET = 11;
constexpr size_t TIME_COLON1_INDEX = 13;
constexpr size_t MINUTE_DIGITS_OFFSET = 14;
constexpr size_t TIME_COLON2_INDEX = 16;
constexpr size_t SECOND_DIGITS_OFFSET = 17;
constexpr size_t FRACTION_DIGITS_SCALE = 6;
constexpr int MAX_HOUR_INCLUSIVE = 23;
constexpr int MAX_MINUTE_INCLUSIVE = 59;
constexpr int MAX_SECOND_INCLUSIVE_LEAP = 60;

constexpr std::int64_t SECONDS_PER_MINUTE = 60;
constexpr std::int64_t SECONDS_PER_HOUR = 3600;

bool ParseFixedDigits(const char *p, size_t n, int &out)
{
    int value = 0;
    for (size_t i = 0; i < n; ++i)
    {
        const char c = p[i];
        if (c < '0' || c > '9')
        {
            return false;
        }
        value = (value * DECIMAL_RADIX) + (c - '0');
    }
    out = value;
    return true;
}

/** Returns the kind of a format that has a hand-written parser of its own, or Generic for any other. */
TimestampFormatKind FastPathKind(std::string_view format)
{
    constexpr std::string_view ISO_T{"%FT%T"};
    constexpr std::string_view ISO_SPACE{"%F %T"};
    constexpr std::string_view ISO_T_OFFSET{"%FT%T%Ez"};
    constexpr std::string_view ISO_SPACE_OFFSET{"%F %T%Ez"};
    // Both `%e` (space-padded day) and `%d` (zero-padded day) are
    // legitimate RFC 3164 spellings. RFC 3164 §4.1.2 mandates the
    // space-padded shape, but many implementations (and stdlib
    // `strftime` on Windows) emit the zero-padded shape, so the fast
    // path handles both. The parser accepts either input verbatim so
    // either format string routes to the same manual parser.
    constexpr std::string_view SYSLOG_E{"%b %e %H:%M:%S"};
    constexpr std::string_view SYSLOG_D{"%b %d %H:%M:%S"};
    if (format == ISO_T)
    {
        return TimestampFormatKind::Iso8601T;
    }
    if (format == ISO_SPACE)
    {
        return TimestampFormatKind::Iso8601Space;
    }
    if (format == ISO_T_OFFSET)
    {
        return TimestampFormatKind::Iso8601TOffset;
    }
    if (format == ISO_SPACE_OFFSET)
    {
        return TimestampFormatKind::Iso8601SpaceOffset;
    }
    if (format == SYSLOG_E || format == SYSLOG_D)
    {
        return TimestampFormatKind::SyslogRfc3164NoYear;
    }
    return TimestampFormatKind::Generic;
}

/** Which suffix may follow the date, time, and fraction. */
enum class IsoZoneSuffix : std::uint8_t
{
    /** Nothing, or a Z. */
    NoneOrUtc,
    /** A required numeric offset. */
    Offset,
};

/** Parses all of @p digits as a decimal number. */
bool ParseDigits(std::string_view digits, int &out)
{
    return ParseFixedDigits(digits.data(), digits.size(), out);
}

/** Parses +h, +hh, +hh:mm, or +hhmm covering all of @p sv. */
bool ParseIsoOffset(std::string_view sv, std::int64_t &offsetSeconds)
{
    constexpr size_t COMPACT_DIGITS = 4;
    constexpr size_t MAX_HOUR_DIGITS = 2;
    if (sv.empty() || (sv.front() != '+' && sv.front() != '-'))
    {
        return false;
    }
    const std::int64_t sign = sv.front() == '-' ? -1 : 1;
    sv.remove_prefix(1);

    int hours = 0;
    int minutes = 0;
    const size_t colon = sv.find(':');
    if (colon != std::string_view::npos)
    {
        if (colon == 0 || colon > MAX_HOUR_DIGITS || sv.size() != colon + 1 + 2 ||
            !ParseDigits(sv.substr(0, colon), hours) || !ParseDigits(sv.substr(colon + 1), minutes))
        {
            return false;
        }
    }
    else if (sv.size() == COMPACT_DIGITS)
    {
        if (!ParseDigits(sv.substr(0, 2), hours) || !ParseDigits(sv.substr(2), minutes))
        {
            return false;
        }
    }
    else if (sv.empty() || sv.size() > MAX_HOUR_DIGITS || !ParseDigits(sv, hours))
    {
        return false;
    }
    if (hours > MAX_HOUR_INCLUSIVE || minutes > MAX_MINUTE_INCLUSIVE)
    {
        return false;
    }
    offsetSeconds = sign * ((hours * SECONDS_PER_HOUR) + (minutes * SECONDS_PER_MINUTE));
    return true;
}

bool ParseIsoTimestamp(std::string_view sv, char dateTimeSep, IsoZoneSuffix zoneSuffix, TimeStamp &out)
{
    // Layout: YYYY-MM-DDsHH:MM:SS[.fraction]
    constexpr size_t PREFIX_LEN = 19;
    if (sv.size() < PREFIX_LEN)
    {
        return false;
    }

    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (!ParseFixedDigits(sv.data() + 0, 4, year) || sv[ISO_DASH1_INDEX] != '-' ||
        !ParseFixedDigits(sv.data() + MONTH_DIGITS_OFFSET, 2, month) || sv[ISO_DASH2_INDEX] != '-' ||
        !ParseFixedDigits(sv.data() + DAY_DIGITS_OFFSET, 2, day) || sv[DATE_TIME_SEPARATOR_INDEX] != dateTimeSep ||
        !ParseFixedDigits(sv.data() + HOUR_DIGITS_OFFSET, 2, hour) || sv[TIME_COLON1_INDEX] != ':' ||
        !ParseFixedDigits(sv.data() + MINUTE_DIGITS_OFFSET, 2, minute) || sv[TIME_COLON2_INDEX] != ':' ||
        !ParseFixedDigits(sv.data() + SECOND_DIGITS_OFFSET, 2, second))
    {
        return false;
    }

    size_t cursor = PREFIX_LEN;
    int64_t fractionalUs = 0;
    // ISO 8601 permits both `.` and `,` as the decimal separator. Java
    // Logback, log4j2, and SLF4J emit `,`. Digits past microseconds are
    // consumed and truncated, so nanosecond timestamps keep their instant.
    if (cursor < sv.size() && (sv[cursor] == '.' || sv[cursor] == ','))
    {
        ++cursor;
        size_t digits = 0;
        while (cursor < sv.size() && sv[cursor] >= '0' && sv[cursor] <= '9')
        {
            if (digits < FRACTION_DIGITS_SCALE)
            {
                fractionalUs = (fractionalUs * DECIMAL_RADIX) + (sv[cursor] - '0');
            }
            ++digits;
            ++cursor;
        }
        if (digits == 0)
        {
            return false;
        }
        for (size_t i = digits; i < FRACTION_DIGITS_SCALE; ++i)
        {
            fractionalUs *= DECIMAL_RADIX;
        }
    }

    std::int64_t offsetSeconds = 0;
    if (zoneSuffix == IsoZoneSuffix::Offset)
    {
        if (!ParseIsoOffset(sv.substr(cursor), offsetSeconds))
        {
            return false;
        }
    }
    else
    {
        if (cursor < sv.size() && sv[cursor] == 'Z')
        {
            ++cursor;
        }
        if (cursor != sv.size())
        {
            return false;
        }
    }

    // Accept second == 60 to match `date::parse("%T")` leap-second handling.
    if (hour > MAX_HOUR_INCLUSIVE || minute > MAX_MINUTE_INCLUSIVE || second > MAX_SECOND_INCLUSIVE_LEAP)
    {
        return false;
    }

    const date::year_month_day ymd{
        date::year{year}, date::month{static_cast<unsigned>(month)}, date::day{static_cast<unsigned>(day)}
    };
    if (!ymd.ok())
    {
        return false;
    }

    const auto days = date::sys_days{ymd};
    const auto totalUs = std::chrono::duration_cast<std::chrono::microseconds>(days.time_since_epoch()) +
                         std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::seconds{(hour * 3600) + (minute * 60) + second - offsetSeconds}
                         ) +
                         std::chrono::microseconds{fractionalUs};
    out = TimeStamp{totalUs};
    // Syntactically valid Y/M/D/H/M/S/fraction is success; the POSIX epoch
    // and pre-1970 timestamps are valid outputs, not failures.
    return true;
}

} // namespace

bool TryParseIsoTimestamp(std::string_view sv, char dateTimeSep, TimeStamp &out)
{
    return ParseIsoTimestamp(sv, dateTimeSep, IsoZoneSuffix::NoneOrUtc, out);
}

bool TryParseIsoOffsetTimestamp(std::string_view sv, char dateTimeSep, TimeStamp &out)
{
    return ParseIsoTimestamp(sv, dateTimeSep, IsoZoneSuffix::Offset, out);
}

namespace
{

/** Year and month used by the RFC 3164 year-injection heuristic. */
struct AssumedNowFields
{
    int year;
    unsigned month;
};

AssumedNowFields AssumedNow(std::chrono::system_clock::time_point referenceNow)
{
    const auto today = date::floor<date::days>(referenceNow);
    const date::year_month_day ymd{today};
    return AssumedNowFields{
        .year = static_cast<int>(ymd.year()),
        .month = static_cast<unsigned>(ymd.month()),
    };
}

/**
 * Case-sensitive lookup for RFC 3164's English month abbreviations.
 * Returns 1-based month index (1..12) or 0 on miss.
 */
unsigned MatchSyslogMonth(std::string_view sv) noexcept
{
    static constexpr size_t MONTH_ABBREV_LEN = 3;
    if (sv.size() < MONTH_ABBREV_LEN)
    {
        return 0;
    }
    static constexpr std::array<std::string_view, 12> MONTHS = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    for (unsigned i = 0; i < MONTHS.size(); ++i)
    {
        const auto &m = MONTHS[i];
        if (sv[0] == m[0] && sv[1] == m[1] && sv[2] == m[2])
        {
            return i + 1;
        }
    }
    return 0;
}

} // namespace

bool TryParseSyslogRfc3164Timestamp(
    std::string_view sv, TimeStamp &out, std::chrono::system_clock::time_point referenceNow
)
{
    // Shortest legal shape is `Jan  1 00:00:00` = 15 bytes.
    constexpr size_t MIN_LEN = 15;
    if (sv.size() < MIN_LEN)
    {
        return false;
    }

    const unsigned month = MatchSyslogMonth(sv);
    if (month == 0)
    {
        return false;
    }
    if (sv[3] != ' ')
    {
        return false;
    }

    // Skip the optional second padding space (`%e` shape); accept
    // both `Aug  8 ...` (space-padded) and `Aug 08 ...` (zero-padded).
    size_t cursor = 4;
    if (sv[cursor] == ' ')
    {
        ++cursor;
    }

    // Day: 1 or 2 digits.
    if (cursor >= sv.size() || sv[cursor] < '0' || sv[cursor] > '9')
    {
        return false;
    }
    int day = sv[cursor] - '0';
    ++cursor;
    if (cursor < sv.size() && sv[cursor] >= '0' && sv[cursor] <= '9')
    {
        day = (day * DECIMAL_RADIX) + (sv[cursor] - '0');
        ++cursor;
    }
    constexpr int MAX_DAY_INCLUSIVE = 31;
    if (day < 1 || day > MAX_DAY_INCLUSIVE)
    {
        return false;
    }

    if (cursor >= sv.size() || sv[cursor] != ' ')
    {
        return false;
    }
    ++cursor;

    // Fixed-width `HH:MM:SS` tail (8 bytes) with a hard end-of-input.
    constexpr size_t TIME_TAIL_LEN = 8;
    if (sv.size() - cursor != TIME_TAIL_LEN)
    {
        return false;
    }
    // Byte offsets within the `HH:MM:SS` tail.
    constexpr size_t MINUTE_COLON_OFFSET = 2;
    constexpr size_t MINUTE_DIGITS_TAIL_OFFSET = 3;
    constexpr size_t SECOND_COLON_OFFSET = 5;
    constexpr size_t SECOND_DIGITS_TAIL_OFFSET = 6;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (!ParseFixedDigits(sv.data() + cursor, 2, hour))
    {
        return false;
    }
    if (sv[cursor + MINUTE_COLON_OFFSET] != ':')
    {
        return false;
    }
    if (!ParseFixedDigits(sv.data() + cursor + MINUTE_DIGITS_TAIL_OFFSET, 2, minute))
    {
        return false;
    }
    if (sv[cursor + SECOND_COLON_OFFSET] != ':')
    {
        return false;
    }
    if (!ParseFixedDigits(sv.data() + cursor + SECOND_DIGITS_TAIL_OFFSET, 2, second))
    {
        return false;
    }
    if (hour > MAX_HOUR_INCLUSIVE || minute > MAX_MINUTE_INCLUSIVE || second > MAX_SECOND_INCLUSIVE_LEAP)
    {
        return false;
    }

    // Standard RFC 3164 year-injection heuristic: if the parsed
    // month is later in the year than the wall-clock month, the log
    // record is from the prior calendar year (Dec log viewed in Jan
    // is the classic case). Falls apart for records older than 12
    // months, which is an inherent limitation of the year-less
    // header shape -- users with longer horizons should switch the
    // sender to RFC 5424.
    const AssumedNowFields now = AssumedNow(referenceNow);
    int year = now.year;
    if (month > now.month)
    {
        year -= 1;
    }

    const date::year_month_day ymd{date::year{year}, date::month{month}, date::day{static_cast<unsigned>(day)}};
    if (!ymd.ok())
    {
        return false;
    }

    const auto days = date::sys_days{ymd};
    const auto totalUs = std::chrono::duration_cast<std::chrono::microseconds>(days.time_since_epoch()) +
                         std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::seconds{(hour * 3600) + (minute * 60) + second}
                         );
    out = TimeStamp{totalUs};
    return true;
}

namespace
{

/**
 * Removes fraction digits past microseconds. date::from_stream stops at
 * microseconds and would read the digits left over as a UTC offset.
 */
void DropExcessFractionDigits(std::string &text)
{
    const auto isDigit = [](char ch) { return ch >= '0' && ch <= '9'; };
    for (size_t dot = text.find('.'); dot != std::string::npos; dot = text.find('.', dot + 1))
    {
        const size_t firstDigit = dot + 1;
        size_t end = firstDigit;
        while (end < text.size() && isDigit(text[end]))
        {
            ++end;
        }
        if (end - firstDigit > FRACTION_DIGITS_SCALE)
        {
            text.erase(firstDigit + FRACTION_DIGITS_SCALE, end - firstDigit - FRACTION_DIGITS_SCALE);
        }
    }
}

} // namespace

namespace
{

/** The largest number a run of digits in a format may spell. */
constexpr int MAX_FORMAT_NUMBER = 9;

/**
 * True for the specifiers that the general parser reads as a signed number: century, year and ISO week-based year.
 * A sign uses one character of the field width, so a width of 1 leaves none for the digits, which the parser then reads
 * without any limit.
 */
constexpr bool IsSignedField(char specifier) noexcept
{
    return specifier == 'C' || specifier == 'F' || specifier == 'Y' || specifier == 'G';
}

/**
 * True if every number in the format is small, every field width is at least 1, and a signed field is at least 2 wide.
 *
 * After some specifiers the general parser reads the following digits as a field width. A long run overflows the int
 * that holds the width, a wide field lets the text give seconds that overflow the 64-bit microsecond count, and a width
 * of zero, or of one for a signed field, means no limit at all. A number above 9 would let a field take enough digits
 * to overflow, and no real format needs one.
 */
bool FormatNumbersAreSmall(const std::string &format) noexcept
{
    int value = 0;
    bool isWidth = false;
    for (std::size_t i = 0; i < format.size(); ++i)
    {
        const char ch = format[i];
        if (ch >= '0' && ch <= '9')
        {
            if (value == 0 && i > 0 && format[i - 1] == '%')
            {
                isWidth = true;
            }
            value = (value * DECIMAL_RADIX) + (ch - '0');
            if (value > MAX_FORMAT_NUMBER)
            {
                return false;
            }
            continue;
        }
        if (isWidth && (value == 0 || (value == 1 && IsSignedField(ch))))
        {
            return false;
        }
        value = 0;
        isWidth = false;
    }
    return !isWidth || value != 0;
}

} // namespace

namespace
{

/** Runs date::from_stream on text. The caller has checked the format with FormatNumbersAreSmall. */
bool ParseWithDate(std::string_view sv, const std::string &format, TimestampParseScratch &scratch, TimeStamp &out)
{
    scratch.str.assign(sv.data(), sv.size());
    DropExcessFractionDigits(scratch.str);
    scratch.stream.clear();
    scratch.stream.str(scratch.str);
    out = TimeStamp{};
    // Call `date::from_stream` directly rather than using
    // `scratch.stream >> date::parse(format, out)`. The latter expands inside
    // `date::parse` to an unqualified `from_stream(...)` call, which becomes
    // ambiguous in C++20+/libc++ where `std::chrono::from_stream` is also a
    // viable overload for `std::chrono::time_point<system_clock, microseconds>`.
    date::from_stream(scratch.stream, format.c_str(), out);
    // The POSIX epoch and pre-1970 timestamps are valid outputs, so only the
    // stream state decides. Unconsumed input means the format did not match.
    return !scratch.stream.fail() && scratch.stream.peek() == std::istringstream::traits_type::eof();
}

} // namespace

bool TryParseGenericTimestamp(
    std::string_view sv, const std::string &format, TimestampParseScratch &scratch, TimeStamp &out
)
{
    if (!FormatNumbersAreSmall(format))
    {
        out = TimeStamp{};
        return false;
    }
    return ParseWithDate(sv, format, scratch, out);
}

namespace
{

using Step = TimestampFormat::Step;
using Op = TimestampFormat::Step::Op;

/** date::parse reads %S at microsecond precision as at most nine characters: two digits, a point and six digits. */
constexpr unsigned SECOND_FIELD_MAX_CHARS = 9;
/** date::parse reads %Y as an optional sign and digits, four characters in all. */
constexpr unsigned YEAR_FIELD_MAX_CHARS = 4;
constexpr std::int64_t MICROS_PER_SECOND = 1'000'000;
constexpr std::int64_t MICROS_PER_MINUTE = 60 * MICROS_PER_SECOND;
constexpr std::int64_t MICROS_PER_HOUR = 60 * MICROS_PER_MINUTE;
constexpr std::int64_t MICROS_PER_DAY = 24 * MICROS_PER_HOUR;
constexpr std::int64_t MINUTES_PER_HOUR = 60;
constexpr std::size_t NAME_LENGTH = 3;

constexpr std::array<std::string_view, 12> MONTH_NAMES = {
    "Jan",
    "Feb",
    "Mar",
    "Apr",
    "May",
    "Jun",
    "Jul",
    "Aug",
    "Sep",
    "Oct",
    "Nov",
    "Dec",
};
constexpr std::array<std::string_view, 7> WEEKDAY_NAMES = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

[[nodiscard]] constexpr bool IsDigit(char ch) noexcept
{
    return ch >= '0' && ch <= '9';
}

/** The white space of the C locale, which date::parse skips for a space in the format. */
[[nodiscard]] constexpr bool IsSpace(char ch) noexcept
{
    return ch == ' ' || (ch >= '\t' && ch <= '\r');
}

[[nodiscard]] constexpr bool IsLetter(char ch) noexcept
{
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
}

/** The characters date::parse accepts in a zone name for %Z. */
[[nodiscard]] constexpr bool IsZoneNameChar(char ch) noexcept
{
    return IsLetter(ch) || IsDigit(ch) || ch == '_' || ch == '/' || ch == '-' || ch == '+';
}

/** Collects the steps of a format and the fields they set. */
class StepBuilder
{
public:
    /** Marks fields as set. Returns false if one of them was set before, which date::parse checks for equal values. */
    bool Claim(unsigned fields)
    {
        if ((mSeen & fields) != 0)
        {
            return false;
        }
        mSeen |= fields;
        return true;
    }

    void Add(Op op, char literal = 0)
    {
        mSteps.push_back(Step{.op = op, .literal = literal});
    }

    [[nodiscard]] unsigned Seen() const noexcept
    {
        return mSeen;
    }

    [[nodiscard]] std::vector<Step> TakeSteps() noexcept
    {
        return std::move(mSteps);
    }

private:
    std::vector<Step> mSteps;
    unsigned mSeen = 0;
};

// The fields a step sets, as bits, so a field set twice can be found.
constexpr unsigned YEAR_FIELD = 1U << 0U;
constexpr unsigned MONTH_FIELD = 1U << 1U;
constexpr unsigned DAY_FIELD = 1U << 2U;
constexpr unsigned HOUR_FIELD = 1U << 3U;
constexpr unsigned MINUTE_FIELD = 1U << 4U;
constexpr unsigned SECOND_FIELD = 1U << 5U;
constexpr unsigned WEEKDAY_FIELD = 1U << 6U;
constexpr unsigned OFFSET_FIELD = 1U << 7U;
constexpr unsigned ZONE_FIELD = 1U << 8U;
constexpr unsigned DATE_FIELDS = YEAR_FIELD | MONTH_FIELD | DAY_FIELD;

/** Adds the steps of one specifier. Returns false if the steps do not cover it. */
bool CompileSpecifier(char specifier, StepBuilder &builder)
{
    switch (specifier)
    {
    case '%':
        builder.Add(Op::Literal, '%');
        return true;
    case 'Y':
        builder.Add(Op::Year);
        return builder.Claim(YEAR_FIELD);
    case 'm':
        builder.Add(Op::Month);
        return builder.Claim(MONTH_FIELD);
    case 'd':
    case 'e':
        builder.Add(Op::Day);
        return builder.Claim(DAY_FIELD);
    case 'H':
        builder.Add(Op::Hour);
        return builder.Claim(HOUR_FIELD);
    case 'M':
        builder.Add(Op::Minute);
        return builder.Claim(MINUTE_FIELD);
    case 'S':
        builder.Add(Op::Second);
        return builder.Claim(SECOND_FIELD);
    case 'F':
        builder.Add(Op::Year);
        builder.Add(Op::Literal, '-');
        builder.Add(Op::Month);
        builder.Add(Op::Literal, '-');
        builder.Add(Op::Day);
        return builder.Claim(DATE_FIELDS);
    case 'T':
        builder.Add(Op::Hour);
        builder.Add(Op::Literal, ':');
        builder.Add(Op::Minute);
        builder.Add(Op::Literal, ':');
        builder.Add(Op::Second);
        return builder.Claim(HOUR_FIELD | MINUTE_FIELD | SECOND_FIELD);
    case 'b':
    case 'B':
    case 'h':
        builder.Add(Op::MonthName);
        return builder.Claim(MONTH_FIELD);
    case 'a':
    case 'A':
        builder.Add(Op::WeekdayName);
        return builder.Claim(WEEKDAY_FIELD);
    case 'z':
        builder.Add(Op::Offset);
        return builder.Claim(OFFSET_FIELD);
    case 'Z':
        builder.Add(Op::ZoneName);
        return builder.Claim(ZONE_FIELD);
    default:
        return false;
    }
}

/**
 * Turns a format into steps. Returns false if the format uses anything the steps do not cover: another specifier, a
 * field width, a modifier other than %Ez, a field given twice, or no year, month or day.
 */
bool CompileSteps(std::string_view format, std::vector<Step> &steps)
{
    StepBuilder builder;
    for (std::size_t i = 0; i < format.size(); ++i)
    {
        const char ch = format[i];
        if (ch != '%')
        {
            builder.Add(IsSpace(ch) ? Op::Space : Op::Literal, ch);
            continue;
        }
        if (++i == format.size())
        {
            return false;
        }
        if (format[i] == 'E')
        {
            if (i + 1 == format.size() || format[i + 1] != 'z')
            {
                return false;
            }
            ++i;
            builder.Add(Op::OffsetWithColon);
            if (!builder.Claim(OFFSET_FIELD))
            {
                return false;
            }
            continue;
        }
        if (!CompileSpecifier(format[i], builder))
        {
            return false;
        }
    }
    if ((builder.Seen() & DATE_FIELDS) != DATE_FIELDS)
    {
        return false;
    }
    steps = builder.TakeSteps();
    return true;
}

/** Reads minDigits to maxDigits digits, as date::parse reads an unsigned field. */
bool ReadUnsigned(std::string_view text, std::size_t &pos, unsigned minDigits, unsigned maxDigits, int &out)
{
    unsigned count = 0;
    int value = 0;
    while (count < maxDigits && pos < text.size() && IsDigit(text[pos]))
    {
        value = (value * DECIMAL_RADIX) + (text[pos] - '0');
        ++pos;
        ++count;
    }
    if (count < minDigits)
    {
        return false;
    }
    out = value;
    return true;
}

/** Reads an optional sign and at least one digit, maxChars characters in all, as date::parse reads %Y. */
bool ReadSigned(std::string_view text, std::size_t &pos, unsigned maxChars, int &out)
{
    if (pos == text.size())
    {
        return false;
    }
    const char first = text[pos];
    const bool negative = first == '-';
    if (first == '-' || first == '+')
    {
        ++pos;
        --maxChars;
    }
    int value = 0;
    if (!ReadUnsigned(text, pos, 1, maxChars, value))
    {
        return false;
    }
    out = negative ? -value : value;
    return true;
}

/**
 * Reads seconds as date::parse reads %S: digits with at most one point, nine characters in all, at least one. The text
 * has at most six digits after any point, so the value is exact in microseconds.
 */
bool ReadSeconds(std::string_view text, std::size_t &pos, std::int64_t &micros)
{
    unsigned count = 0;
    std::size_t fractionDigits = 0;
    std::int64_t whole = 0;
    std::int64_t fraction = 0;
    bool inFraction = false;
    while (pos < text.size())
    {
        const char ch = text[pos];
        if (ch == '.' && !inFraction)
        {
            inFraction = true;
        }
        else if (IsDigit(ch))
        {
            if (inFraction)
            {
                fraction = (fraction * DECIMAL_RADIX) + (ch - '0');
                ++fractionDigits;
            }
            else
            {
                whole = (whole * DECIMAL_RADIX) + (ch - '0');
            }
        }
        else
        {
            break;
        }
        ++pos;
        if (++count == SECOND_FIELD_MAX_CHARS)
        {
            break;
        }
    }
    if (count == 0)
    {
        return false;
    }
    for (std::size_t i = fractionDigits; i < FRACTION_DIGITS_SCALE; ++i)
    {
        fraction *= DECIMAL_RADIX;
    }
    micros = (whole * MICROS_PER_SECOND) + fraction;
    return true;
}

/** The result of reading one step. Defer means only date::parse can tell, because the locale decides. */
enum class StepResult : std::uint8_t
{
    Ok,
    Fail,
    Defer,
};

/**
 * Reads an English three-letter name. A name spelled another way, such as a full name or in other letter case, is left
 * to date::parse, whose answer depends on the locale. Text that starts with neither a letter nor a space is no name.
 */
template <std::size_t N>
StepResult ReadName(std::string_view text, std::size_t &pos, const std::array<std::string_view, N> &names, int &index)
{
    if (pos == text.size())
    {
        return StepResult::Fail;
    }
    if (!IsLetter(text[pos]))
    {
        return IsSpace(text[pos]) ? StepResult::Defer : StepResult::Fail;
    }
    const std::size_t left = text.size() - pos;
    if (left >= NAME_LENGTH && (left == NAME_LENGTH || !IsLetter(text[pos + NAME_LENGTH])))
    {
        const std::string_view candidate = text.substr(pos, NAME_LENGTH);
        for (std::size_t i = 0; i < names.size(); ++i)
        {
            if (candidate == names[i])
            {
                pos += NAME_LENGTH;
                index = static_cast<int>(i);
                return StepResult::Ok;
            }
        }
    }
    return StepResult::Defer;
}

/** Reads %z (hh and an optional mm) or %Ez (h or hh, and an optional :mm), with an optional sign, as date::parse does.
 */
bool ReadOffset(std::string_view text, std::size_t &pos, bool withColon, std::int64_t &offsetMinutes)
{
    const bool negative = pos < text.size() && text[pos] == '-';
    if (pos < text.size() && (text[pos] == '-' || text[pos] == '+'))
    {
        ++pos;
    }
    int hours = 0;
    int minutes = 0;
    if (!withColon)
    {
        if (!ReadUnsigned(text, pos, 2, 2, hours))
        {
            return false;
        }
        if (pos < text.size() && IsDigit(text[pos]) && !ReadUnsigned(text, pos, 2, 2, minutes))
        {
            return false;
        }
    }
    else
    {
        if (!ReadUnsigned(text, pos, 1, 2, hours))
        {
            return false;
        }
        if (pos < text.size() && text[pos] == ':')
        {
            ++pos;
            if (!ReadUnsigned(text, pos, 2, 2, minutes))
            {
                return false;
            }
        }
    }
    offsetMinutes = (static_cast<std::int64_t>(hours) * MINUTES_PER_HOUR) + minutes;
    if (negative)
    {
        offsetMinutes = -offsetMinutes;
    }
    return true;
}

/** True if a point in text is followed by more than six digits, which date::parse would need cut first. */
bool HasExcessFractionDigits(std::string_view text) noexcept
{
    for (std::size_t dot = text.find('.'); dot != std::string_view::npos; dot = text.find('.', dot + 1))
    {
        std::size_t end = dot + 1;
        while (end < text.size() && IsDigit(text[end]))
        {
            ++end;
        }
        if (end - dot - 1 > FRACTION_DIGITS_SCALE)
        {
            return true;
        }
    }
    return false;
}

/** The fields the steps read. A field the format does not set keeps its default, as in date::parse. */
struct ParsedFields
{
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    std::int64_t secondMicros = 0;
    int weekday = -1;
    std::int64_t offsetMinutes = 0;
};

/** Runs one step. Defer means only date::parse can tell. */
StepResult RunStep(const Step &step, std::string_view text, std::size_t &pos, ParsedFields &fields)
{
    bool ok = true;
    switch (step.op)
    {
    case Op::Literal:
        ok = pos < text.size() && text[pos] == step.literal;
        ++pos;
        break;
    case Op::Space:
        while (pos < text.size() && IsSpace(text[pos]))
        {
            ++pos;
        }
        break;
    case Op::Year:
        ok = ReadSigned(text, pos, YEAR_FIELD_MAX_CHARS, fields.year);
        break;
    case Op::Month:
        ok = ReadUnsigned(text, pos, 1, 2, fields.month);
        break;
    case Op::Day:
        ok = ReadUnsigned(text, pos, 1, 2, fields.day);
        break;
    case Op::Hour:
        ok = ReadUnsigned(text, pos, 1, 2, fields.hour);
        break;
    case Op::Minute:
        ok = ReadUnsigned(text, pos, 1, 2, fields.minute);
        break;
    case Op::Second:
        ok = ReadSeconds(text, pos, fields.secondMicros);
        break;
    case Op::MonthName:
    {
        int index = 0;
        const StepResult result = ReadName(text, pos, MONTH_NAMES, index);
        fields.month = index + 1;
        return result;
    }
    case Op::WeekdayName:
        return ReadName(text, pos, WEEKDAY_NAMES, fields.weekday);
    case Op::Offset:
    case Op::OffsetWithColon:
        ok = ReadOffset(text, pos, step.op == Op::OffsetWithColon, fields.offsetMinutes);
        break;
    case Op::ZoneName:
    {
        const std::size_t start = pos;
        while (pos < text.size() && IsZoneNameChar(text[pos]))
        {
            ++pos;
        }
        ok = pos != start;
        break;
    }
    }
    return ok ? StepResult::Ok : StepResult::Fail;
}

/** Checks the fields as date::parse does and turns them into a time. Returns false for an impossible date or time. */
bool FieldsToTimestamp(const ParsedFields &fields, TimeStamp &out)
{
    if (fields.month < 1 || fields.hour > MAX_HOUR_INCLUSIVE || fields.minute > MAX_MINUTE_INCLUSIVE ||
        fields.secondMicros >= MICROS_PER_MINUTE)
    {
        return false;
    }
    const date::year_month_day ymd{
        date::year{fields.year},
        date::month{static_cast<unsigned>(fields.month)},
        date::day{static_cast<unsigned>(fields.day)}
    };
    if (!ymd.ok())
    {
        return false;
    }
    const date::sys_days days{ymd};
    if (fields.weekday >= 0 && date::weekday{days}.c_encoding() != static_cast<unsigned>(fields.weekday))
    {
        return false;
    }
    const std::int64_t micros = (static_cast<std::int64_t>(days.time_since_epoch().count()) * MICROS_PER_DAY) +
                                (static_cast<std::int64_t>(fields.hour) * MICROS_PER_HOUR) +
                                (static_cast<std::int64_t>(fields.minute) * MICROS_PER_MINUTE) + fields.secondMicros -
                                (fields.offsetMinutes * MICROS_PER_MINUTE);
    out = TimeStamp{std::chrono::microseconds{micros}};
    return true;
}

} // namespace

TimestampFormat::TimestampFormat(std::string format)
    : mText(std::move(format)), mKind(FastPathKind(mText)), mHasZone(FormatHasZoneSpecifier(mText))
{
    if (mKind != TimestampFormatKind::Generic)
    {
        return;
    }
    if (CompileSteps(mText, mSteps))
    {
        mKind = TimestampFormatKind::Compiled;
        return;
    }
    if (!FormatNumbersAreSmall(mText))
    {
        mKind = TimestampFormatKind::Rejected;
    }
}

const std::string &TimestampFormat::Text() const noexcept
{
    return mText;
}

TimestampFormatKind TimestampFormat::Kind() const noexcept
{
    return mKind;
}

bool TimestampFormat::HasZone() const noexcept
{
    return mHasZone;
}

int TimestampFormat::RunSteps(std::string_view text, TimeStamp &out) const
{
    ParsedFields fields;
    std::size_t pos = 0;
    for (const Step &step : mSteps)
    {
        const StepResult result = RunStep(step, text, pos, fields);
        if (result == StepResult::Defer)
        {
            return -1;
        }
        if (result == StepResult::Fail)
        {
            return 0;
        }
    }
    return pos == text.size() && FieldsToTimestamp(fields, out) ? 1 : 0;
}

bool TimestampFormat::Parse(
    std::string_view text,
    TimestampParseScratch &scratch,
    std::chrono::system_clock::time_point referenceTime,
    TimeStamp &out
) const
{
    switch (mKind)
    {
    case TimestampFormatKind::Iso8601T:
        return TryParseIsoTimestamp(text, 'T', out);
    case TimestampFormatKind::Iso8601Space:
        return TryParseIsoTimestamp(text, ' ', out);
    case TimestampFormatKind::Iso8601TOffset:
        return TryParseIsoOffsetTimestamp(text, 'T', out);
    case TimestampFormatKind::Iso8601SpaceOffset:
        return TryParseIsoOffsetTimestamp(text, ' ', out);
    case TimestampFormatKind::SyslogRfc3164NoYear:
        return TryParseSyslogRfc3164Timestamp(text, out, referenceTime);
    case TimestampFormatKind::Rejected:
        out = TimeStamp{};
        return false;
    case TimestampFormatKind::Compiled:
    {
        std::string_view view = text;
        if (HasExcessFractionDigits(text))
        {
            // date::parse sees the text with the digits past microseconds cut, so the steps read the same text.
            scratch.str.assign(text.data(), text.size());
            DropExcessFractionDigits(scratch.str);
            view = scratch.str;
        }
        const int result = RunSteps(view, out);
        if (result < 0)
        {
            return ParseWithDate(text, mText, scratch, out);
        }
        if (result == 0)
        {
            out = TimeStamp{};
        }
        return result == 1;
    }
    case TimestampFormatKind::Generic:
    default:
        return ParseWithDate(text, mText, scratch, out);
    }
}
} // namespace loglib
