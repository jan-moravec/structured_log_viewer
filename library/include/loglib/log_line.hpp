#pragma once

#include "loglib/log_file.hpp"

#include <chrono>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace loglib
{

/**
 * @brief Represents a timestamp for a log entry.
 *
 * The timestamp is stored with a precision of microseconds, which is generally
 * the highest precision achievable on modern systems for inter-process communication.
 */
using TimeStamp = std::chrono::time_point<std::chrono::system_clock, std::chrono::microseconds>;

/**
 * @brief Represents a value in a log entry.
 *
 * A log value can be one of several types, including string, integer, floating-point,
 * boolean, timestamp, or an empty state (std::monostate).
 */
using LogValue = std::variant<std::string, int64_t, uint64_t, double, bool, TimeStamp, std::monostate>;

/**
 * @brief Map of string keys to their corresponding log values.
 *
 */
using LogMap = std::unordered_map<std::string, LogValue>;

/**
 * @brief Represents a single line in a log or a single log record, consisting of key-value pairs.
 */
class LogLine
{
public:
    /**
     * @brief Constructs a new LogLine with the given key-value pairs.
     *
     * @param values A map of string keys to their corresponding log values.
     */
    LogLine(LogMap values, LogFileReference fileReference);

    // Deleted copy constructor and copy assignment operator for efficiency.
    LogLine(LogLine &) = delete;
    LogLine &operator=(LogLine &) = delete;

    // Defaulted move constructor and move assignment operator.
    LogLine(LogLine &&) = default;
    LogLine &operator=(LogLine &&) = default;

    /**
     * @brief Retrieves the value associated with a given key.
     *
     * @param key The key to search for in the log line.
     * @return LogValue The value associated with the key. If the key is not found,
     *                  returns std::monostate.
     */
    LogValue GetValue(const std::string &key) const;

    /**
     * @brief Sets or updates the value for a given key.
     *
     * @param key The key to associate with the value.
     * @param value The value to set for the given key.
     */
    void SetValue(const std::string &key, LogValue value);

    /**
     * @brief Get all the log line keys.
     *
     * @return std::vector<std::string> Log line keys.
     */
    std::vector<std::string> GetKeys() const;

    /**
     * @brief Retrieves the file reference associated with this log line.
     *
     * @return const LogFileReference& The file reference for this log line.
     */
    const LogFileReference &FileReference() const;

    /**
     * @brief Retrieves the log line values.
     *
     * @return const LogMap& The map of key-value pairs for this log line.
     */
    const LogMap &Values() const;
    LogMap &Values();

private:
    LogMap mValues;
    const LogFileReference mFileReference;
};

} // namespace loglib
