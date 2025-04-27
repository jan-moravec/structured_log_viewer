#pragma once

#include "log_data.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace loglib
{

/**
 * @brief Represents the configuration for log data, including columns and filters.
 */
struct LogConfiguration
{
    /**
     * @brief Enum to specify the type of data in a column.
     */
    enum class Type
    {
        Any, ///< Any type of data.
        Time ///< Time-based data.
    };

    /**
     * @brief Represents a column in the log configuration.
     */
    struct Column
    {
        std::string header;                    ///< The header name of the column.
        std::vector<std::string> keys;         ///< Keys associated with the column.
        std::string printFormat;               ///< Format string for printing the column data.
        Type type = Type::Any;                 ///< Type of the column data.
        std::vector<std::string> parseFormats; ///< Formats for parsing the column data.

        NLOHMANN_DEFINE_TYPE_INTRUSIVE(Column, header, keys, printFormat, type, parseFormats)
    };

    /**
     * @brief Represents a filter for log data.
     */
    struct LogFilter
    {
        /**
         * @brief Enum to specify the type of filter.
         */
        enum class Type
        {
            String, ///< String-based filter.
            Time    ///< Time-based filter.
        };

        /**
         * @brief Enum to specify the matching criteria for the filter.
         */
        enum class Match
        {
            Exactly,           ///< Exact match.
            Contains,          ///< Substring match.
            RegularExpression, ///< Regular expression match.
            Wildcard           ///< Wildcard match.
        };

        Type type;                               ///< Type of the filter.
        int row;                                 ///< Row index to apply the filter.
        std::optional<std::string> filterString; ///< String value for the filter.
        std::optional<Match> matchType;          ///< Matching criteria for the filter.
        std::optional<int64_t> filterBegin;      ///< Start range for time-based filters.
        std::optional<int64_t> filterEnd;        ///< End range for time-based filters.

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

    std::vector<Column> columns;    ///< List of columns in the configuration.
    std::vector<LogFilter> filters; ///< List of filters in the configuration.

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(LogConfiguration, columns, filters)
};

// JSON serialization for LogConfiguration::Type
NLOHMANN_JSON_SERIALIZE_ENUM(
    LogConfiguration::Type, {{LogConfiguration::Type::Any, "any"}, {LogConfiguration::Type::Time, "time"}}
)

// JSON serialization for LogFilter::Type
NLOHMANN_JSON_SERIALIZE_ENUM(
    LogConfiguration::LogFilter::Type,
    {{LogConfiguration::LogFilter::Type::String, "string"}, {LogConfiguration::LogFilter::Type::Time, "time"}}
)

// JSON serialization for LogFilter::Match
NLOHMANN_JSON_SERIALIZE_ENUM(
    LogConfiguration::LogFilter::Match,
    {{LogConfiguration::LogFilter::Match::Exactly, "exactly"},
     {LogConfiguration::LogFilter::Match::Contains, "contains"},
     {LogConfiguration::LogFilter::Match::RegularExpression, "regularExpression"},
     {LogConfiguration::LogFilter::Match::Wildcard, "wildcard"}}
)

/**
 * @brief Manages the log configuration, including loading, saving, and updating it.
 */
class LogConfigurationManager
{
public:
    LogConfigurationManager() = default;

    /**
     * @brief Loads the log configuration from a file.
     * @param path The path to the configuration file.
     * @throws std::runtime_error if the file cannot be opened.
     */
    void Load(const std::filesystem::path &path);

    /**
     * @brief Saves the log configuration to a file.
     * @param path The path to the configuration file.
     * @throws std::runtime_error if the file cannot be opened.
     */
    void Save(const std::filesystem::path &path) const;

    /**
     * @brief Updates the log configuration based on the provided log data.
     * @param logData The log data used to update the configuration.
     */
    void Update(const LogData &logData);

    /**
     * @brief Retrieves the current log configuration.
     * @return A constant reference to the log configuration.
     */
    const LogConfiguration &Configuration() const;

private:
    LogConfiguration mConfiguration;
};

} // namespace loglib
