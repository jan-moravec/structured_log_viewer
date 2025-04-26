#include "loglib/log_processing.hpp"

namespace
{

using namespace loglib;

bool ParseTimestampLine(LogLine &line, const LogConfiguration::Column &column)
{
    std::string errors;

    for (const std::string &key : column.keys)
    {
        LogValue value = line.GetValue(key);
        if (std::holds_alternative<std::string>(value))
        {
            const auto timeStampString = std::get<std::string>(value);
            for (const std::string &format : column.parseFormats)
            {
                try
                {
                    std::istringstream stream{timeStampString};
                    TimeStamp timestamp;
                    stream >> date::parse(format, timestamp);
                    if (timestamp.time_since_epoch().count() > 0)
                    {
                        line.SetValue(key, timestamp);
                        return true;
                    }
                }
                catch (const std::exception &)
                {
                    // Error parsing the timestamp, let's try another format or key if available
                }
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
        if (column.type == LogConfiguration::Type::Time)
        {
            for (auto &line : logData.Lines())
            {
                if (!ParseTimestampLine(line, column))
                {
                    errors.emplace_back(
                        "Failed to parse a timestamp for column '" + column.header + "' from line number " +
                        std::to_string(line.FileReference().GetLineNumber())
                    );
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
