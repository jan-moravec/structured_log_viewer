#pragma once

#include "log_parser.hpp"

#include <simdjson.h>
#include <tsl/robin_map.h>

namespace loglib
{

/**
 * @class JsonParser
 * @brief A parser for processing log files in JSON format.
 *
 * This class extends the LogParser interface to provide functionality
 * for validating and parsing JSON log files.
 */
class JsonParser : public LogParser
{
public:
    /**
     * @brief Checks if the given file is a valid JSON log file.
     *
     * @param file The path to the file to validate.
     * @return True if the file is valid, false otherwise.
     */
    bool IsValid(const std::filesystem::path &file) const override;

    /**
     * @brief Parses the given JSON log file.
     *
     * @param file The path to the file to parse.
     * @return A ParseResult containing the parsed log data.
     */
    ParseResult Parse(const std::filesystem::path &file) const override;

    /**
     * @brief Converts log values to a string representation.
     *
     * @param values The log values to convert.
     * @return A string representation of the log line.
     */
    std::string ToString(const LogMap &values) const override;

private:
    /**
     * @brief Speed parsing with cached type information for keys.
     */
    struct ParseCache
    {
        tsl::robin_map<std::string, simdjson::ondemand::json_type> keyTypes;
        tsl::robin_map<std::string, simdjson::ondemand::number_type> numberTypes;
    };

    /**
     * @brief Parses JSON data object.
     *
     * @param element The simdjson DOM element representing the line.
     * @param keys A set to store unique keys encountered during parsing.
     * @return A map of key-value pairs extracted from the JSON line.
     */
    static LogMap ParseLine(simdjson::ondemand::object &object, ParseCache &cache);
};

} // namespace loglib
