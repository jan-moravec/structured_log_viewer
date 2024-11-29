#include "log_processing.hpp"

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
            for (const std::string &format : column.parseFormats)
            {
                try
                {
                    std::istringstream stream{std::get<std::string>(value)};
                    TimeStamp timestamp;
                    stream >> date::parse(format, timestamp);
                    line.SetExtraValue(key, timestamp);
                    return true;
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

void Initialize()
{
    std::filesystem::path dataPath = std::filesystem::current_path() / std::filesystem::path("tzdata");
    date::set_install(dataPath.string());
    static_cast<void>(date::current_zone()); // Test the database
}

std::string ParseTimestamps(LogData &logData, const LogConfiguration &configuration)
{
    std::string errors;

    for (size_t i = 0; i < configuration.columns.size(); ++i)
    {
        const LogConfiguration::Column &column = configuration.columns[i];
        if (column.type == LogConfiguration::Type::Time)
        {
            for (auto &line : logData.GetLines())
            {
                if (!ParseTimestampLine(*line, column))
                {
                    errors += "Failed to parse a timestamp for column " + std::to_string(i) + " '" + column.header +
                              "' from line: " + line->GetLine() + "\n";
                }
            }
        }
    }

    if (!errors.empty())
    {
        errors.pop_back(); // Last endline
    }

    return errors;
}

int64_t TimeStampToLocalMillisecondsSinceEpoch(TimeStamp timeStamp)
{
    static auto tz = date::current_zone(); // Get the current time zone
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
    static auto tz = date::current_zone(); // Get the current time zone
    const auto localTime = date::local_time<std::chrono::microseconds>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::milliseconds(milliseconds))
    );
    const auto systemTime = tz->to_sys(localTime); // Convert local time to system time
    return std::chrono::time_point_cast<std::chrono::microseconds>(systemTime);
}

} // namespace loglib
