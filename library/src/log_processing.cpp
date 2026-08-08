#include "loglib/log_processing.hpp"

#include "loglib/internal/timestamp_promotion.hpp"

#include <date/date.h>
#include <date/tz.h>
#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>

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

} // namespace

TimestampFormatKind ClassifyTimestampFormat(std::string_view format)
{
    constexpr std::string_view ISO_T{"%FT%T"};
    constexpr std::string_view ISO_SPACE{"%F %T"};
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
        return TimestampFormatKind::Iso8601_T;
    }
    if (format == ISO_SPACE)
    {
        return TimestampFormatKind::Iso8601_Space;
    }
    if (format == SYSLOG_E || format == SYSLOG_D)
    {
        return TimestampFormatKind::SyslogRfc3164NoYear;
    }
    return TimestampFormatKind::Generic;
}

bool TryParseIsoTimestamp(std::string_view sv, char dateTimeSep, TimeStamp &out)
{
    // Layout: YYYY-MM-DDsHH:MM:SS[.fff[fff]]
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
    if (!ParseFixedDigits(sv.data() + 0, 4, year))
    {
        return false;
    }
    if (sv[ISO_DASH1_INDEX] != '-')
    {
        return false;
    }
    if (!ParseFixedDigits(sv.data() + MONTH_DIGITS_OFFSET, 2, month))
    {
        return false;
    }
    if (sv[ISO_DASH2_INDEX] != '-')
    {
        return false;
    }
    if (!ParseFixedDigits(sv.data() + DAY_DIGITS_OFFSET, 2, day))
    {
        return false;
    }
    if (sv[DATE_TIME_SEPARATOR_INDEX] != dateTimeSep)
    {
        return false;
    }
    if (!ParseFixedDigits(sv.data() + HOUR_DIGITS_OFFSET, 2, hour))
    {
        return false;
    }
    if (sv[TIME_COLON1_INDEX] != ':')
    {
        return false;
    }
    if (!ParseFixedDigits(sv.data() + MINUTE_DIGITS_OFFSET, 2, minute))
    {
        return false;
    }
    if (sv[TIME_COLON2_INDEX] != ':')
    {
        return false;
    }
    if (!ParseFixedDigits(sv.data() + SECOND_DIGITS_OFFSET, 2, second))
    {
        return false;
    }

    int64_t fractionalUs = 0;
    // Accept an optional fractional part followed by an optional `Z`.
    // ISO 8601 §4.2.2.4 permits both `.` and `,` as the decimal
    // separator; RFC 3339 pins on `.`, but Java Logback / log4j2 /
    // SLF4J's default PatternLayout and many European locale
    // timestamps emit `,` (e.g. `2024-04-28 04:02:03,123`). The
    // shipped Java regex template captures `[.,]\d+` for exactly
    // this reason, so treating both bytes as equivalent here lets
    // the fast path handle both spellings without adding a new
    // `parseFormats` entry.
    if (sv.size() > PREFIX_LEN)
    {
        size_t cursor = PREFIX_LEN;
        if (sv[cursor] == '.' || sv[cursor] == ',')
        {
            const size_t fractionStart = cursor + 1;
            const size_t maxFractionEnd = std::min(sv.size(), fractionStart + FRACTION_DIGITS_SCALE);
            size_t fractionEnd = fractionStart;
            while (fractionEnd < maxFractionEnd && sv[fractionEnd] >= '0' && sv[fractionEnd] <= '9')
            {
                ++fractionEnd;
            }
            const size_t fractionLen = fractionEnd - fractionStart;
            // Reject empty and longer-than-microsecond fractions here.
            if (fractionLen == 0)
            {
                return false;
            }
            for (size_t i = fractionStart; i < fractionEnd; ++i)
            {
                fractionalUs = (fractionalUs * DECIMAL_RADIX) + (sv[i] - '0');
            }
            for (size_t i = fractionLen; i < FRACTION_DIGITS_SCALE; ++i)
            {
                fractionalUs *= DECIMAL_RADIX;
            }
            cursor = fractionEnd;
        }
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
                             std::chrono::seconds{(hour * 3600) + (minute * 60) + second}
                         ) +
                         std::chrono::microseconds{fractionalUs};
    out = TimeStamp{totalUs};
    // Syntactically valid Y/M/D/H/M/S/fraction is success; the POSIX epoch
    // and pre-1970 timestamps are valid outputs, not failures.
    return true;
}

namespace
{

/// Process-lifetime cache of the "assumed current" year and month used
/// by the RFC 3164 year-injection heuristic. Sampled once at first
/// use (viewer sessions rarely span year boundaries, and a stale
/// value at most shifts a Dec / Jan boundary by one calendar year --
/// same failure mode the heuristic already has for logs older than
/// twelve months).
struct AssumedNowFields
{
    int year;
    unsigned month;
};

AssumedNowFields AssumedNow()
{
    static const AssumedNowFields CACHED = []() {
        const auto today = date::floor<date::days>(std::chrono::system_clock::now());
        const date::year_month_day ymd{today};
        return AssumedNowFields{
            .year = static_cast<int>(ymd.year()),
            .month = static_cast<unsigned>(ymd.month()),
        };
    }();
    return CACHED;
}

/// Case-sensitive lookup for RFC 3164's English month abbreviations.
/// Returns 1-based month index (`1..12`) or 0 on miss.
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

bool TryParseSyslogRfc3164Timestamp(std::string_view sv, TimeStamp &out)
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
    const AssumedNowFields now = AssumedNow();
    int year = now.year;
    if (month > now.month)
    {
        year -= 1;
    }

    const date::year_month_day ymd{
        date::year{year}, date::month{month}, date::day{static_cast<unsigned>(day)}
    };
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

bool TryParseGenericTimestamp(
    std::string_view sv, const std::string &format, TimestampParseScratch &scratch, TimeStamp &out
)
{
    scratch.str.assign(sv.data(), sv.size());
    scratch.stream.clear();
    scratch.stream.str(scratch.str);
    out = TimeStamp{};
    // Call `date::from_stream` directly rather than using
    // `scratch.stream >> date::parse(format, out)`. The latter expands inside
    // `date::parse` to an unqualified `from_stream(...)` call, which becomes
    // ambiguous in C++20+/libc++ where `std::chrono::from_stream` is also a
    // viable overload for `std::chrono::time_point<system_clock, microseconds>`.
    date::from_stream(scratch.stream, format.c_str(), out);
    // Stream-fail bit alone is the success signal: the POSIX epoch and
    // pre-1970 timestamps are valid outputs.
    return !scratch.stream.fail();
}

bool TryParseTimestamp(
    std::string_view sv,
    const std::string &format,
    TimestampFormatKind kind,
    TimestampParseScratch &scratch,
    TimeStamp &out
)
{
    switch (kind)
    {
    case TimestampFormatKind::Iso8601_T:
        return TryParseIsoTimestamp(sv, 'T', out);
    case TimestampFormatKind::Iso8601_Space:
        return TryParseIsoTimestamp(sv, ' ', out);
    case TimestampFormatKind::SyslogRfc3164NoYear:
        return TryParseSyslogRfc3164Timestamp(sv, out);
    case TimestampFormatKind::Generic:
    default:
        return TryParseGenericTimestamp(sv, format, scratch, out);
    }
}

const date::time_zone *CurrentZone()
{
    static const date::time_zone *tz = date::current_zone();
    return tz;
}

void Initialize(const std::filesystem::path &tzdata)
{
    // Validate the path up front. `date::set_install` only fails lazily,
    // and `date::current_zone()` memoizes the first successful result, so
    // a later `Initialize(bad_path)` in a process that already initialized
    // would silently succeed. Checking here keeps the precondition
    // independent of date's internal cache.
    if (!std::filesystem::exists(tzdata) || !std::filesystem::is_directory(tzdata))
    {
        throw std::runtime_error("tzdata directory does not exist: " + tzdata.string());
    }
    date::set_install(tzdata.string());
    static_cast<void>(date::current_zone());
}

namespace
{

/// Builds the per-line scratch state shared by both `BackfillTimestampColumn`
/// overloads. Returns `false` when @p lines is empty, in which case the
/// caller should bail out (the spec arrays are not built).
bool MakeBackfillState(
    const LogConfiguration::Column &column,
    std::span<LogLine> lines,
    std::array<internal::TimeColumnSpec, 1> &specsOut,
    std::vector<std::optional<LastValidTimestampParse>> &lastValidOut,
    std::vector<internal::LastTimestampBytesHit> &bytesHitsOut
)
{
    if (lines.empty())
    {
        return false;
    }

    const KeyIndex &keyIndex = lines.front().Keys();
    internal::TimeColumnSpec spec;
    spec.keyIds.reserve(column.keys.size());
    for (const std::string &key : column.keys)
    {
        spec.keyIds.push_back(keyIndex.Find(key));
    }
    spec.parseFormats = column.parseFormats;
    spec.formatKinds.reserve(spec.parseFormats.size());
    for (const std::string &format : spec.parseFormats)
    {
        spec.formatKinds.push_back(ClassifyTimestampFormat(format));
    }
    specsOut[0] = std::move(spec);
    lastValidOut.assign(1, std::nullopt);
    bytesHitsOut.assign(1, internal::LastTimestampBytesHit{});
    return true;
}

} // namespace

namespace
{

/// Returns an empty view: `BackfillTimestampColumn` over a `LogLine`
/// span flows through `PromoteLineTimestamps`, which now resolves the
/// per-line owned-bytes arena via the line's `LineSource` directly
/// (`source->ResolveOwnedBytes(offset, length, lineId)`). The
/// `ownedArena` parameter only matters during Stage B / Stage C of the
/// parser pipeline, where the per-batch staging buffer carries
/// `OwnedString` payloads before they are rebased onto the canonical
/// source arena. This backfill path runs after the pipeline, when
/// every payload is already source-relative.
std::string_view OwnedArenaForBackfill(const LogLine & /*line*/) noexcept
{
    return std::string_view{};
}

} // namespace

std::vector<std::string> BackfillTimestampColumn(const LogConfiguration::Column &column, std::span<LogLine> lines)
{
    std::vector<std::string> errors;
    std::array<internal::TimeColumnSpec, 1> specs;
    std::vector<std::optional<LastValidTimestampParse>> lastValid;
    std::vector<internal::LastTimestampBytesHit> bytesHits;
    if (!MakeBackfillState(column, lines, specs, lastValid, bytesHits))
    {
        return errors;
    }

    TimestampParseScratch scratch;
    for (auto &line : lines)
    {
        const std::string_view ownedArena = OwnedArenaForBackfill(line);
        if (!internal::PromoteLineTimestamps(line, specs, lastValid, bytesHits, scratch, ownedArena))
        {
            errors.emplace_back(
                fmt::format(
                    "Failed to parse a timestamp for column '{}' from line number {}", column.header, line.LineId()
                )
            );
        }
    }
    return errors;
}

void BackfillTimestampColumn(
    const LogConfiguration::Column &column, std::span<LogLine> lines, BackfillErrors discardErrors
)
{
    static_cast<void>(discardErrors);
    std::array<internal::TimeColumnSpec, 1> specs;
    std::vector<std::optional<LastValidTimestampParse>> lastValid;
    std::vector<internal::LastTimestampBytesHit> bytesHits;
    if (!MakeBackfillState(column, lines, specs, lastValid, bytesHits))
    {
        return;
    }

    TimestampParseScratch scratch;
    for (auto &line : lines)
    {
        const std::string_view ownedArena = OwnedArenaForBackfill(line);
        static_cast<void>(internal::PromoteLineTimestamps(line, specs, lastValid, bytesHits, scratch, ownedArena));
    }
}

std::vector<std::string> ParseTimestamps(LogData &logData, const LogConfiguration &configuration)
{
    std::vector<std::string> errors;

    for (const auto &column : configuration.columns)
    {
        if (column.type == LogConfiguration::Type::Time)
        {
            auto columnErrors = BackfillTimestampColumn(column, logData.Lines());
            if (!columnErrors.empty())
            {
                errors.reserve(errors.size() + columnErrors.size());
                std::ranges::move(columnErrors, std::back_inserter(errors));
            }
        }
    }

    return errors;
}

int64_t TimeStampToLocalMillisecondsSinceEpoch(TimeStamp timeStamp)
{
    const auto zonedTime = date::zoned_time{CurrentZone(), timeStamp};
    const auto localTime = zonedTime.get_local_time();
    return std::chrono::duration_cast<std::chrono::milliseconds>(localTime.time_since_epoch()).count();
}

int64_t UtcMicrosecondsToLocalMilliseconds(int64_t microseconds)
{
    const std::chrono::time_point<std::chrono::system_clock, std::chrono::microseconds> utcTime{
        std::chrono::microseconds{microseconds}
    };
    const date::zoned_time localTime{CurrentZone(), utcTime};
    return std::chrono::duration_cast<std::chrono::milliseconds>(localTime.get_local_time().time_since_epoch()).count();
}

TimeStamp LocalMillisecondsSinceEpochToTimeStamp(int64_t milliseconds)
{
    const auto localTime = date::local_time<std::chrono::microseconds>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::milliseconds(milliseconds))
    );
    const auto systemTime = CurrentZone()->to_sys(localTime);
    return std::chrono::time_point_cast<std::chrono::microseconds>(systemTime);
}

int64_t LocalMicrosecondsSinceEpochToUtc(int64_t localMicroseconds, const date::time_zone *zone)
{
    if (zone == nullptr)
    {
        return localMicroseconds;
    }
    const date::local_time<std::chrono::microseconds> localTime{std::chrono::microseconds{localMicroseconds}};
    try
    {
        // `choose::earliest` resolves DST edge cases without
        // throwing: ambiguous fall-back hour -> earlier candidate;
        // spring-forward gap -> the transition boundary. We do
        // NOT catch `nonexistent_local_time` /
        // `ambiguous_local_time` because the `choose` overload
        // never throws them.
        const auto systemTime = zone->to_sys(localTime, date::choose::earliest);
        return systemTime.time_since_epoch().count();
    }
    catch (const std::exception &)
    {
        // Reachable for far-future dates past the tzdata table
        // and corrupt zone entries. Falling back to the naive
        // value keeps the Goto Timestamp slot exception-safe.
        return localMicroseconds;
    }
}

int64_t LocalMicrosecondsSinceEpochToUtc(int64_t localMicroseconds)
{
    return LocalMicrosecondsSinceEpochToUtc(localMicroseconds, CurrentZone());
}

std::string UtcMicrosecondsToDateTimeString(int64_t microseconds)
{
    const std::chrono::time_point<std::chrono::system_clock, std::chrono::microseconds> utcTime{
        std::chrono::microseconds{microseconds}
    };
    const date::zoned_time localTime{CurrentZone(), std::chrono::round<std::chrono::milliseconds>(utcTime)};
    return date::format("%F %T", localTime);
}

std::string TimeStampToDateTimeString(TimeStamp timeStamp)
{
    const date::zoned_time localTime{CurrentZone(), std::chrono::round<std::chrono::milliseconds>(timeStamp)};
    return date::format("%F %T", localTime);
}

} // namespace loglib
