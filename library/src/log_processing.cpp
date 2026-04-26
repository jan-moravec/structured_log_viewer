#include "loglib/log_processing.hpp"

#include <date/date.h>
#include <date/tz.h>
#include <fmt/format.h>

#include <iterator>
#include <sstream>
#include <string>

namespace loglib
{

namespace
{

/**
 * @brief Tries a single `(keyId, format)` pair against @p line.
 *
 * Returns true (and overwrites the line's value at @p keyId with a `TimeStamp`)
 * iff the value at @p keyId is a string-like alternative that `date::parse`
 * accepts under @p format and the resulting timestamp is non-epoch. Used by
 * both the public KeyId-keyed `ParseTimestampLine` overload and the legacy
 * column-keyed wrapper that `BackfillTimestampColumn` exposes.
 */
bool TryParseTimestampOnce(LogLine &line, KeyId keyId, const std::string &format)
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
    // istringstream still needs an owning string; the construction is unavoidable for the
    // streaming-stdlib parser API. Once this hot path matters more we can swap in a
    // string_view-aware parser (PRD §4.2.21 future work).
    std::istringstream stream{std::string(*timestampString)};
    TimeStamp timestamp;
    stream >> date::parse(format, timestamp);
    if (stream && timestamp.time_since_epoch().count() > 0)
    {
        line.SetValue(keyId, timestamp);
        return true;
    }
    return false;
}

} // namespace

bool ParseTimestampLine(
    LogLine &line,
    std::span<const KeyId> keyIds,
    std::span<const std::string> parseFormats,
    std::optional<LastValidTimestampParse> &lastValid
)
{
    // Fast path: try the (keyId, format) pair that worked on the previous line first.
    // For files that use a single timestamp format throughout, this collapses the per-line
    // work to one date::parse + one LogLine::GetValue.
    if (lastValid.has_value())
    {
        if (TryParseTimestampOnce(line, lastValid->keyId, lastValid->format))
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
            if (TryParseTimestampOnce(line, keyId, format))
            {
                lastValid = LastValidTimestampParse{keyId, format};
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
