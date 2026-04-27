#include "loglib/log_processing.hpp"

#include <date/date.h>
#include <date/tz.h>
#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>

namespace loglib
{

namespace
{

/// Reads @p n digit bytes from @p p into @p out. Returns false on any
/// non-digit byte. Caller guarantees @p n bytes are readable.
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

/// Thread-local scratch used by `ParseTimestampLine`. The streaming pipeline
/// uses per-`WorkerState` scratch instead; this is for the legacy whole-data
/// `BackfillTimestampColumn` path and ad-hoc callers.
TimestampParseScratch &ThreadScratch()
{
    thread_local TimestampParseScratch scratch;
    return scratch;
}

/// Tries a single `(keyId, format, kind)` triple against @p line. On success
/// overwrites the line's value at @p keyId with the parsed `TimeStamp`.
bool TryParseTimestampOnce(
    LogLine &line,
    KeyId keyId,
    const std::string &format,
    TimestampFormatKind kind,
    TimestampParseScratch &scratch
)
{
    if (keyId == kInvalidKeyId)
    {
        return false;
    }
    LogValue value = line.GetValue(keyId);
    const auto timestampString = AsStringView(value);
    if (!timestampString.has_value())
    {
        return false;
    }
    TimeStamp timestamp;
    if (!TryParseTimestamp(*timestampString, format, kind, scratch, timestamp))
    {
        return false;
    }
    line.SetValue(keyId, timestamp);
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
    //         0123456789012345678
    //                            ^19
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
        // Reject empty fractions (`12:34:56.`) and anything beyond 6 digits (timezone
        // offsets, sub-microsecond precision); those fall back to `date::parse`.
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

    // `date::year_month_day::ok()` validates the date but not H:M:S. Accept
    // second == 60 to match `date::parse("%T", …)` leap-second handling.
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
                             std::chrono::seconds{hour * 3600 + minute * 60 + second}
                         ) +
                         std::chrono::microseconds{fractionalUs};
    out = TimeStamp{totalUs};
    // Treat non-positive epochs as parse failures, mirroring the slow-path sentinel.
    return out.time_since_epoch().count() > 0;
}

bool TryParseGenericTimestamp(
    std::string_view sv, const std::string &format, TimestampParseScratch &scratch, TimeStamp &out
)
{
    // Reuse the scratch's std::string and istringstream so we amortise both the
    // copy and the stringbuf across calls. `clear()` resets the previous parse's
    // fail/eof bits before we swap in new bytes.
    scratch.str.assign(sv.data(), sv.size());
    scratch.stream.clear();
    scratch.stream.str(scratch.str);
    out = TimeStamp{};
    scratch.stream >> date::parse(format, out);
    return !scratch.stream.fail() && out.time_since_epoch().count() > 0;
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

namespace
{

/// Per-line back-fill body shared by `BackfillTimestampColumn`. Walks the
/// `(keyIds × parseFormats)` matrix, trying the cached pair in @p lastValid
/// first; on success, promotes the line's value to `TimeStamp` and updates
/// the cache. Streaming uses `detail::PromoteLineTimestamps` directly.
bool ParseTimestampLine(
    LogLine &line,
    std::span<const KeyId> keyIds,
    std::span<const std::string> parseFormats,
    std::optional<LastValidTimestampParse> &lastValid
)
{
    TimestampParseScratch &scratch = ThreadScratch();

    if (lastValid.has_value())
    {
        if (TryParseTimestampOnce(line, lastValid->keyId, lastValid->format, lastValid->kind, scratch))
        {
            return true;
        }
    }

    for (const KeyId keyId : keyIds)
    {
        if (keyId == kInvalidKeyId)
        {
            continue;
        }
        for (const std::string &format : parseFormats)
        {
            const TimestampFormatKind kind = ClassifyTimestampFormat(format);
            if (TryParseTimestampOnce(line, keyId, format, kind, scratch))
            {
                lastValid = LastValidTimestampParse{keyId, format, kind};
                return true;
            }
        }
    }

    return false;
}

} // namespace

const date::time_zone *CurrentZone()
{
    // Must be called after `Initialize` has installed the tzdata database.
    // Cached here so every formatter/converter shares one zone instance.
    static const date::time_zone *tz = date::current_zone();
    return tz;
}

void Initialize(const std::filesystem::path &tzdata)
{
    date::set_install(tzdata.string());
    static_cast<void>(date::current_zone()); // Test the database
}

std::vector<std::string> BackfillTimestampColumn(const LogConfiguration::Column &column, std::vector<LogLine> &lines)
{
    // Per-column timestamp back-fill shared by `ParseTimestamps` (whole-data) and
    // `LogTable::AppendBatch` (mid-stream after a new time column is auto-promoted).
    std::vector<std::string> errors;
    if (lines.empty())
    {
        return errors;
    }

    // Resolve `column.keys` to KeyIds once so the per-line inner loop avoids
    // `KeyIndex::Find` per call. Unknown keys become `kInvalidKeyId` and are skipped.
    const KeyIndex &keyIndex = lines.front().Keys();
    std::vector<KeyId> keyIds;
    keyIds.reserve(column.keys.size());
    for (const std::string &key : column.keys)
    {
        keyIds.push_back(keyIndex.Find(key));
    }

    std::optional<LastValidTimestampParse> lastValid;
    for (auto &line : lines)
    {
        if (!ParseTimestampLine(line, keyIds, column.parseFormats, lastValid))
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
