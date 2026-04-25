#pragma once

#include "log_data.hpp"
#include "log_line.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace loglib
{

/**
 * @brief Result of parsing a log file.
 *
 * This structure contains the parsed log data and errors that occurred during parsing.
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
     * @brief Convert a log line to a string representation.
     *
     * Implementations should produce the parser's native serialization (e.g. a
     * JSON object string for `JsonParser`). Operates on a `LogLine` rather
     * than the raw value collection so that implementations can resolve KeyIds
     * via the line's bound `KeyIndex` (PRD req. 4.1.14).
     *
     * @param line The log line to convert.
     * @return std::string The string representation of the log line.
     */
    virtual std::string ToString(const LogLine &line) const = 0;
};

} // namespace loglib
