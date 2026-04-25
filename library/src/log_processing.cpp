#include "loglib/log_processing.hpp"

#include <date/date.h>
#include <date/tz.h>
#include <fmt/format.h>

#include <iterator>
#include <sstream>
#include <string>

namespace
{

using namespace loglib;

bool ParseTimestampLine(LogLine &line, const std::string &key, const std::string &format)
{
    LogValue value = line.GetValue(key);
    const auto timestampString = AsStringView(value);
    if (timestampString.has_value())
    {
        // istringstream still needs an owning string; the construction is unavoidable for the
        // streaming-stdlib parser API. Once this hot path matters more we can swap in a
        // string_view-aware parser (PRD §4.2.21 future work).
        std::istringstream stream{std::string(*timestampString)};
        TimeStamp timestamp;
        stream >> date::parse(format, timestamp);
        if (stream && timestamp.time_since_epoch().count() > 0)
        {
            line.SetValue(key, timestamp);
            return true;
        }
    }

    return false;
}

struct LastValidTimestampParse
{
    std::string key;
    std::string format;
};

bool ParseTimestampLine(
    LogLine &line, const LogConfiguration::Column &column, std::optional<LastValidTimestampParse> &lastValid
)
{
    if (lastValid.has_value())
    {
        if (ParseTimestampLine(line, lastValid->key, lastValid->format))
        {
            return true;
        }
    }

    for (const std::string &key : column.keys)
    {
        for (const std::string &format : column.parseFormats)
        {
            if (ParseTimestampLine(line, key, format))
            {
                lastValid = {key, format};
                return true;
            }
        }
    }

    return false;
}

} // namespace

namespace loglib
{

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
    std::optional<LastValidTimestampParse> lastValid;
    for (auto &line : lines)
    {
        if (!ParseTimestampLine(line, column, lastValid))
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
