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
        value = (value * 10) + (c - '0');
    }
    out = value;
    return true;
}

} // namespace

TimestampFormatKind ClassifyTimestampFormat(std::string_view format)
{
    constexpr std::string_view ISO_T{"%FT%T"};
    constexpr std::string_view ISO_SPACE{"%F %T"};
    if (format == ISO_T)
    {
        return TimestampFormatKind::Iso8601_T;
    }
    if (format == ISO_SPACE)
    {
        return TimestampFormatKind::Iso8601_Space;
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
    if (sv[4] != '-')
    {
        return false;
    }
    if (!ParseFixedDigits(sv.data() + 5, 2, month))
    {
        return false;
    }
    if (sv[7] != '-')
    {
        return false;
    }
    if (!ParseFixedDigits(sv.data() + 8, 2, day))
    {
        return false;
    }
    if (sv[10] != dateTimeSep)
    {
        return false;
    }
    if (!ParseFixedDigits(sv.data() + 11, 2, hour))
    {
        return false;
    }
    if (sv[13] != ':')
    {
        return false;
    }
    if (!ParseFixedDigits(sv.data() + 14, 2, minute))
    {
        return false;
    }
    if (sv[16] != ':')
    {
        return false;
    }
    if (!ParseFixedDigits(sv.data() + 17, 2, second))
    {
        return false;
    }

    int64_t fractionalUs = 0;
    if (sv.size() > PREFIX_LEN)
    {
        if (sv[PREFIX_LEN] != '.')
        {
            return false;
        }
        const size_t fractionStart = PREFIX_LEN + 1;
        const size_t maxFractionEnd = std::min(sv.size(), fractionStart + 6);
        size_t fractionEnd = fractionStart;
        while (fractionEnd < maxFractionEnd && sv[fractionEnd] >= '0' && sv[fractionEnd] <= '9')
        {
            ++fractionEnd;
        }
        const size_t fractionLen = fractionEnd - fractionStart;
        // Empty / >6-digit fractions fall back to `date::parse`.
        if (fractionLen == 0 || fractionEnd != sv.size())
        {
            return false;
        }
        for (size_t i = fractionStart; i < fractionEnd; ++i)
        {
            fractionalUs = (fractionalUs * 10) + (sv[i] - '0');
        }
        for (size_t i = fractionLen; i < 6; ++i)
        {
            fractionalUs *= 10;
        }
    }

    // Accept second == 60 to match `date::parse("%T")` leap-second handling.
    if (hour > 23 || minute > 59 || second > 60)
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
            errors.emplace_back(fmt::format(
                "Failed to parse a timestamp for column '{}' from line number {}", column.header, line.LineId()
            ));
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
        if (column.type == LogConfiguration::Type::time)
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
