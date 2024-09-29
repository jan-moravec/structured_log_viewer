#pragma once

#include "log_data.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace loglib
{

struct LogConfiguration
{
    enum class Type
    {
        Any,
        Time
    };

    struct Column
    {
        std::string header;
        std::vector<std::string> keys;
        std::string printFormat;
        Type type = Type::Any;
        std::vector<std::string> parseFormats;

        NLOHMANN_DEFINE_TYPE_INTRUSIVE(Column, header, keys, printFormat, type, parseFormats)
    };

    struct LogFilter
    {
        enum class Type
        {
            String,
            Time
        };

        enum class Match
        {
            Exactly,
            Contains,
            RegularExpression,
            Wildcard,
        };

        Type type;
        int row;
        std::optional<std::string> filterString;
        std::optional<Match> matchType;

        // Custom serialization functions for LogFilter
        friend void to_json(nlohmann::json &j, const LogFilter &lf)
        {
            j = nlohmann::json{
                {"type", lf.type}, // Type enum handled by NLOHMANN_JSON_SERIALIZE_ENUM
                {"row", lf.row}
            };

            // Handle std::optional<std::string>
            if (lf.filterString)
            {
                j["filterString"] = lf.filterString.value();
            }

            // Handle std::optional<Match>
            if (lf.matchType)
            {
                j["matchType"] = lf.matchType.value(); // Match enum handled by NLOHMANN_JSON_SERIALIZE_ENUM
            }
        }

        // Custom deserialization function for LogFilter
        friend void from_json(const nlohmann::json &j, LogFilter &lf)
        {
            j.at("type").get_to(lf.type); // Type enum handled by NLOHMANN_JSON_SERIALIZE_ENUM
            j.at("row").get_to(lf.row);

            // Handle std::optional<std::string>
            if (j.contains("filterString") && !j.at("filterString").is_null())
            {
                lf.filterString = j.at("filterString").get<std::string>();
            }
            else
            {
                lf.filterString = std::nullopt;
            }

            // Handle std::optional<Match>
            if (j.contains("matchType") && !j.at("matchType").is_null())
            {
                lf.matchType = j.at("matchType").get<LogFilter::Match>();
            }
            else
            {
                lf.matchType = std::nullopt;
            }
        }
    };

    std::vector<Column> columns;
    std::vector<LogFilter> filters;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(LogConfiguration, columns, filters)
};

NLOHMANN_JSON_SERIALIZE_ENUM(
    LogConfiguration::Type, {{LogConfiguration::Type::Any, "any"}, {LogConfiguration::Type::Time, "time"}}
)

NLOHMANN_JSON_SERIALIZE_ENUM(
    LogConfiguration::LogFilter::Type,
    {{LogConfiguration::LogFilter::Type::String, "string"}, {LogConfiguration::LogFilter::Type::Time, "time"}}
)

NLOHMANN_JSON_SERIALIZE_ENUM(
    LogConfiguration::LogFilter::Match,
    {{LogConfiguration::LogFilter::Match::Exactly, "exactly"},
     {LogConfiguration::LogFilter::Match::Contains, "contains"},
     {LogConfiguration::LogFilter::Match::RegularExpression, "regularExpression"},
     {LogConfiguration::LogFilter::Match::Wildcard, "wildcard"}}
)

void UpdateConfiguration(LogConfiguration &configuration, const LogData &logData);
void SerializeConfiguration(const std::filesystem::path &path, const LogConfiguration &configuration);
LogConfiguration DeserializeConfiguration(const std::filesystem::path &path);

} // namespace loglib
