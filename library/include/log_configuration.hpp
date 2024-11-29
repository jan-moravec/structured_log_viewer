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
        std::optional<int64_t> filterBegin;
        std::optional<int64_t> filterEnd;

        // Custom serialization functions for LogFilter
        friend void to_json(nlohmann::json &j, const LogFilter &lf)
        {
            j = nlohmann::json{
                {"type", lf.type}, // Type enum handled by NLOHMANN_JSON_SERIALIZE_ENUM
                {"row", lf.row}
            };

            if (lf.filterString)
            {
                j["filterString"] = lf.filterString.value();
            }

            if (lf.filterBegin)
            {
                j["filterBegin"] = lf.filterBegin.value();
            }

            if (lf.filterEnd)
            {
                j["filterEnd"] = lf.filterEnd.value();
            }

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

            if (j.contains("filterString") && !j.at("filterString").is_null())
            {
                lf.filterString = j.at("filterString").get<std::string>();
            }
            else
            {
                lf.filterString = std::nullopt;
            }

            if (j.contains("matchType") && !j.at("matchType").is_null())
            {
                lf.matchType = j.at("matchType").get<LogFilter::Match>();
            }
            else
            {
                lf.matchType = std::nullopt;
            }

            if (j.contains("filterBegin") && !j.at("filterBegin").is_null())
            {
                lf.filterBegin = j.at("filterBegin").get<int64_t>();
            }
            else
            {
                lf.filterBegin = std::nullopt;
            }

            if (j.contains("filterEnd") && !j.at("filterEnd").is_null())
            {
                lf.filterEnd = j.at("filterEnd").get<int64_t>();
            }
            else
            {
                lf.filterEnd = std::nullopt;
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
