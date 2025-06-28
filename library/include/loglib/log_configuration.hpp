#pragma once

#include "log_data.hpp"

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
        any, ///< Any type of data.
        time ///< Time-based data.
    };

    /**
     * @brief Represents a column in the log configuration.
     */
    struct Column
    {
        std::string header;                    ///< The header name of the column.
        std::vector<std::string> keys;         ///< Keys associated with the column.
        std::string printFormat;               ///< Format string for printing the column data.
        Type type = Type::any;                 ///< Type of the column data.
        std::vector<std::string> parseFormats; ///< Formats for parsing the column data.
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
            string, ///< String-based filter.
            time    ///< Time-based filter.
        };

        /**
         * @brief Enum to specify the matching criteria for the filter.
         */
        enum class Match
        {
            exactly,           ///< Exact match.
            contains,          ///< Substring match.
            regularExpression, ///< Regular expression match.
            wildcard           ///< Wildcard match.
        };

        Type type;                               ///< Type of the filter.
        int row;                                 ///< Row index to apply the filter.
        std::optional<std::string> filterString; ///< String value for the filter.
        std::optional<Match> matchType;          ///< Matching criteria for the filter.
        std::optional<int64_t> filterBegin;      ///< Start range for time-based filters.
        std::optional<int64_t> filterEnd;        ///< End range for time-based filters.
    };

    std::vector<Column> columns;    ///< List of columns in the configuration.
    std::vector<LogFilter> filters; ///< List of filters in the configuration.
};

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
