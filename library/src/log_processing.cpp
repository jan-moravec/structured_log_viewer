#include "log_processing.hpp"

namespace
{

bool ParseTimestampsLineKey(loglib::LogLine &line, const std::string &key, const std::vector<std::string> &formats)
{
    loglib::LogValue value = line.GetValue(key);
    if (std::holds_alternative<std::string>(value))
    {
        for (const std::string &format : formats)
        {
            try
            {
                std::istringstream stream{std::get<std::string>(value)};
                loglib::TimeStamp timestamp;
                stream >> date::parse(format, timestamp);
                line.SetExtraValue(key, timestamp);
                return true;
            }
            catch (const std::exception &e)
            {
                // Error parsing the timestamp, let's try another format or key if available
            }
        }
    }

    return false;
}

std::string ParseTimestampsLine(loglib::LogLine &line, const std::vector<loglib::LogConfiguration::Column> &columns)
{
    std::string errors;

    for (const auto &column : columns)
    {
        if (column.type == loglib::LogConfiguration::Type::Time)
        {
            for (const std::string &key : column.keys)
            {
                if (ParseTimestampsLineKey(line, key, column.parseFormats))
                {
                    return "";
                }
            }

            errors += "Failed to parse a timestamp from line :" + line.GetLine() + "\n";
        }
    }

    return errors;
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

    for (const auto &line : logData.GetLines())
    {
        errors += ParseTimestampsLine(*line, configuration.columns);
    }

    if (!errors.empty())
    {
        errors.pop_back(); // Last endline
    }

    return errors;
}

} // namespace loglib
