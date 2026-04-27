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

/**
 * @brief Pure-numeric fixed-length digit parser.
 *
 * Reads @p n bytes starting at @p p, requires every byte to be `'0'..'9'`,
 * and writes the decoded integer into @p out. Returns `false` on any non-
 * digit byte. Caller must guarantee @p n bytes are readable; the function
 * does not bounds-check.
 */
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

/**
 * @brief Process-wide thread-local scratch used by the public
 *        `ParseTimestampLine` so callers that did not opt into per-instance
 *        scratch (the legacy `BackfillTimestampColumn` whole-data path,
 *        ad-hoc tests, …) still get the per-call allocation savings.
 *
 * Stage B of the streaming pipeline does *not* go through this — it owns
 * one `TimestampParseScratch` per `WorkerState` so the same thread running
 * different parses cannot pollute the scratch across them.
 */
TimestampParseScratch &ThreadScratch()
{
    thread_local TimestampParseScratch scratch;
    return scratch;
}

/**
 * @brief Tries a single `(keyId, format, kind)` triple against @p line.
 *
 * Returns true (and overwrites the line's value at @p keyId with a
 * `TimeStamp`) iff the value at @p keyId is a string-like alternative that
 * the dispatcher accepts under @p format / @p kind and the resulting
 * timestamp is non-epoch. Routed through `TryParseTimestamp` so the
 * fast-path branch (`Iso8601_T` / `Iso8601_Space`) and the slow-path
 * branch (`Generic` → `date::parse`) are picked once at the dispatcher,
 * not per call site.
 */
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
    // Conservative match — only the two ISO-8601 shapes we hand-rolled in
    // `TryParseIsoTimestamp`. Anything else (including a leading/trailing
    // space or a `%Ez` suffix) falls back to `date::parse`. The constexpr
    // string_views avoid any per-call allocation; the comparison itself is
    // a length check + memcmp.
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
        // Reject either no digits at all (`12:34:56.`) or trailing garbage past the
        // 6-digit cap (timezone offsets, sub-microsecond precision). Anything we
        // cannot represent at microsecond precision falls back to `date::parse`.
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

    // Range checks. `date::year_month_day::ok()` validates the date itself
    // (including leap-day-in-non-leap-year), but does not check H:M:S. We
    // accept second == 60 to mirror `date::parse("%T", …)` for leap seconds.
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
    // Match the legacy `time_since_epoch().count() > 0` rejection so callers can keep
    // treating "non-positive" as a parse failure (the legacy slow-path reused the same
    // sentinel to detect "stream did not actually parse anything").
    return out.time_since_epoch().count() > 0;
}

bool TryParseGenericTimestamp(
    std::string_view sv, const std::string &format, TimestampParseScratch &scratch, TimeStamp &out
)
{
    // istringstream still needs an owning string buffer for date::parse, but the
    // caller-supplied scratch lets us amortise both the std::string copy and the
    // istringstream's internal buffer across calls. `clear()` resets the eof / fail
    // bits a previous parse may have set, and `str()` swaps in the new bytes
    // without reconstructing the stringbuf.
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

bool ParseTimestampLine(
    LogLine &line,
    std::span<const KeyId> keyIds,
    std::span<const std::string> parseFormats,
    std::optional<LastValidTimestampParse> &lastValid
)
{
    TimestampParseScratch &scratch = ThreadScratch();

    // Fast path: try the (keyId, format) pair that worked on the previous line first.
    // For files that use a single timestamp format throughout, this collapses the per-line
    // work to one date::parse + one LogLine::GetValue.
    if (lastValid.has_value())
    {
        if (TryParseTimestampOnce(line, lastValid->keyId, lastValid->format, lastValid->kind, scratch))
        {
            return true;
        }
    }

    // Slow path: walk the full matrix. Updates `lastValid` to the winning pair so
    // subsequent calls take the fast path.
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

const date::time_zone *CurrentZone()
{
    // date::current_zone() depends on the tzdata database that `Initialize` installs, so it
    // must not be called before Initialize returns. Caching here in a single place keeps
    // every formatter/converter using the same zone instance.
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
    // Shared per-column timestamp back-fill body. Used by:
    //   - ParseTimestamps (whole-data, legacy single-shot path), and
    //   - LogTable::AppendBatch (mid-stream back-fill after a new time column is auto-promoted
    //     from a key first observed in a streaming batch — PRD req. 4.1.13b).
    // The LastValidTimestampParse cache survives only across calls within the same column
    // walk; resetting it at function entry matches the legacy semantics and keeps the
    // back-fill path independent of caller state.
    std::vector<std::string> errors;
    if (lines.empty())
    {
        return errors;
    }

    // Resolve column.keys → KeyIds once at function entry so the per-line inner loop is
    // KeyId-keyed (`LogLine::GetValue(KeyId)` linear scan over the small sorted pair vector,
    // no `KeyIndex::Find` per call). Unknown keys land as kInvalidKeyId and are silently
    // skipped by `ParseTimestampLine`.
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
