#include "loglib/log_processing.hpp"

#include <date/date.h>
#include <date/tz.h>
#include <fmt/format.h>

namespace
{

using namespace loglib;

bool ParseTimestampLine(LogLine &line, const std::string &key, const std::string &format)
{
    LogValue value = line.GetValue(key);
    if (std::holds_alternative<std::string>(value))
    {
        const auto timeStampString = std::get<std::string>(value);
        std::istringstream stream{timeStampString};
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

void Initialize(const std::filesystem::path &tzdata)
{
    date::set_install(tzdata.string());
    static_cast<void>(date::current_zone()); // Test the database
}

std::vector<std::string> ParseTimestamps(LogData &logData, const LogConfiguration &configuration)
{
    std::vector<std::string> errors;

    for (size_t i = 0; i < configuration.columns.size(); ++i)
    {
        const LogConfiguration::Column &column = configuration.columns[i];
        if (column.type == LogConfiguration::Type::time)
        {
            std::optional<LastValidTimestampParse> lastValid;
            for (auto &line : logData.Lines())
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
        }
    }

    return errors;
}

int64_t TimeStampToLocalMillisecondsSinceEpoch(TimeStamp timeStamp)
{
    static auto tz = date::current_zone();
    const auto zonedTime = date::zoned_time{tz, timeStamp};
    const auto localTime = zonedTime.get_local_time();
    return std::chrono::duration_cast<std::chrono::milliseconds>(localTime.time_since_epoch()).count();
}

int64_t UtcMicrosecondsToLocalMilliseconds(int64_t microseconds)
{
    static auto tz = date::current_zone();
    std::chrono::time_point<std::chrono::system_clock, std::chrono::microseconds> utcTime{
        std::chrono::microseconds{microseconds}
    };
    const date::zoned_time localTime{tz, utcTime};
    return std::chrono::duration_cast<std::chrono::milliseconds>(localTime.get_local_time().time_since_epoch()).count();
}

TimeStamp LocalMillisecondsSinceEpochToTimeStamp(int64_t milliseconds)
{
    static auto tz = date::current_zone();
    const auto localTime = date::local_time<std::chrono::microseconds>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::milliseconds(milliseconds))
    );
    const auto systemTime = tz->to_sys(localTime);
    return std::chrono::time_point_cast<std::chrono::microseconds>(systemTime);
}

std::string UtcMicrosecondsToDateTimeString(int64_t microseconds)
{
    static auto tz = date::current_zone();
    std::chrono::time_point<std::chrono::system_clock, std::chrono::microseconds> utcTime{
        std::chrono::microseconds{microseconds}
    };
    const date::zoned_time localTime{tz, std::chrono::round<std::chrono::milliseconds>(utcTime)};
    return date::format("%F %T", localTime);
}

std::string TimeStampToDateTimeString(TimeStamp timeStamp)
{
    static auto tz = date::current_zone();
    const date::zoned_time localTime{tz, std::chrono::round<std::chrono::milliseconds>(timeStamp)};
    return date::format("%F %T", localTime);
}

} // namespace loglib
