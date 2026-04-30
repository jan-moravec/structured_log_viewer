#include "loglib/log_processing.hpp"

#include "loglib/internal/timestamp_promotion.hpp"
#include "loglib/log_file.hpp"

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
        value = value * 10 + (c - '0');
    }
    out = value;
    return true;
}

} // namespace

TimestampFormatKind ClassifyTimestampFormat(std::string_view format)
{
    constexpr std::string_view kIsoT{"%FT%T"};
    constexpr std::string_view kIsoSpace{"%F %T"};
    if (format == kIsoT)
    {
        return TimestampFormatKind::Iso8601_T;
    }
    if (format == kIsoSpace)
    {
        return TimestampFormatKind::Iso8601_Space;
    }
    return TimestampFormatKind::Generic;
}

bool TryParseIsoTimestamp(std::string_view sv, char dateTimeSep, TimeStamp &out)
{
    // Layout: YYYY-MM-DDsHH:MM:SS[.fff[fff]]
    constexpr size_t kPrefixLen = 19;
    if (sv.size() < kPrefixLen)
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
    if (sv.size() > kPrefixLen)
    {
        if (sv[kPrefixLen] != '.')
        {
            return false;
        }
        const size_t fractionStart = kPrefixLen + 1;
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
            fractionalUs = fractionalUs * 10 + (sv[i] - '0');
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
    const auto totalUs =
        std::chrono::duration_cast<std::chrono::microseconds>(days.time_since_epoch()) +
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::seconds{hour * 3600 + minute * 60 + second}
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
    scratch.stream >> date::parse(format, out);
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
    std::array<detail::TimeColumnSpec, 1> &specsOut,
    std::vector<std::optional<LastValidTimestampParse>> &lastValidOut,
    std::vector<detail::LastTimestampBytesHit> &bytesHitsOut
)
{
    if (lines.empty())
    {
        return false;
    }

    const KeyIndex &keyIndex = lines.front().Keys();
    detail::TimeColumnSpec spec;
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
    bytesHitsOut.assign(1, detail::LastTimestampBytesHit{});
    return true;
}

} // namespace

namespace
{

/// Pulls the owned-string arena view from the line's referenced `LogFile`.
/// Post-stream lines have their `OwnedString` payloads rebased onto this
/// arena (Stage C handles streaming; the synchronous `LogLine` ctor writes
/// directly to it). Returns an empty view when the line has no file (only
/// happens in test fixtures).
std::string_view OwnedArenaForBackfill(const LogLine &line) noexcept
{
    const LogFile *file = line.FileReference().GetFile();
    return file != nullptr ? file->OwnedStringsView() : std::string_view{};
}

} // namespace

std::vector<std::string> BackfillTimestampColumn(const LogConfiguration::Column &column, std::span<LogLine> lines)
{
    std::vector<std::string> errors;
    std::array<detail::TimeColumnSpec, 1> specs;
    std::vector<std::optional<LastValidTimestampParse>> lastValid;
    std::vector<detail::LastTimestampBytesHit> bytesHits;
    if (!MakeBackfillState(column, lines, specs, lastValid, bytesHits))
    {
        return errors;
    }

    TimestampParseScratch scratch;
    for (auto &line : lines)
    {
        const std::string_view ownedArena = OwnedArenaForBackfill(line);
        if (!detail::PromoteLineTimestamps(line, specs, lastValid, bytesHits, scratch, ownedArena))
        {
            errors.emplace_back(fmt::format(
                "Failed to parse a timestamp for column '{}' from line number {}",
                column.header,
                line.FileReference().GetLineNumber()
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
    std::array<detail::TimeColumnSpec, 1> specs;
    std::vector<std::optional<LastValidTimestampParse>> lastValid;
    std::vector<detail::LastTimestampBytesHit> bytesHits;
    if (!MakeBackfillState(column, lines, specs, lastValid, bytesHits))
    {
        return;
    }

    TimestampParseScratch scratch;
    for (auto &line : lines)
    {
        const std::string_view ownedArena = OwnedArenaForBackfill(line);
        static_cast<void>(detail::PromoteLineTimestamps(line, specs, lastValid, bytesHits, scratch, ownedArena));
    }
}

std::vector<std::string> ParseTimestamps(LogData &logData, const LogConfiguration &configuration)
{
    std::vector<std::string> errors;

    for (size_t i = 0; i < configuration.columns.size(); ++i)
    {
        const LogConfiguration::Column &column = configuration.columns[i];
        if (column.type == LogConfiguration::Type::time)
        {
            auto columnErrors = BackfillTimestampColumn(column, logData.Lines());
            if (!columnErrors.empty())
            {
                errors.reserve(errors.size() + columnErrors.size());
                std::move(columnErrors.begin(), columnErrors.end(), std::back_inserter(errors));
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
