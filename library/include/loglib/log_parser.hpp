#pragma once

#include "log_data.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace loglib
{

/**
 * @brief Result of parsing a log file.
 *
 * This structure contains the parsed log data and any errors that occurred during parsing.
 *
 */
struct ParseResult
{
    LogData data;
    std::vector<std::string> errors;
};

/**
 * @brief Interface for log parsers.
 *
 * This interface defines the methods that any log parser must implement.
 * It includes methods for checking if a file is valid and for parsing the file.
 *
 */
class LogParser
{
public:
    virtual ~LogParser() = default;

    /**
     * @brief Check if the given file is valid for parsing.
     *
     * This method should be implemented to check if the file can be parsed by this parser.
     *
     * @param file The path to the log file.
     * @return true if the file is valid, false otherwise.
     */
    virtual bool IsValid(const std::filesystem::path &file) const = 0;

    /**
     * @brief Parse the given log file.
     *
     * This method should be implemented to parse the log file and return the result.
     *
     * @param file The path to the log file.
     * @return ParseResult containing the parsed data and any errors that occurred during parsing.
     */
    virtual ParseResult Parse(const std::filesystem::path &file) const = 0;

    /**
     * @brief Convert a log values to a string representation.
     *
     * This method should be implemented to convert a log values to a string format.
     *
     * @param values The log values to convert.
     * @return std::string The string representation of the log line.
     */
    virtual std::string ToString(const LogMap &values) const = 0;
};

} // namespace loglib
