#pragma once

#include "log_file.hpp"
#include "log_line.hpp"

#include <memory>
#include <string>
#include <vector>

namespace loglib
{

/**
 * @brief Collection of log data loaded from log files.
 *
 */
class LogData
{
public:
    /**
     * @brief Empty data collection.
     *
     */
    LogData() = default;

    /**
     * @brief Log data collection.
     *
     * @param files Log file.
     * @param lines Log lines.
     * @param keys Log keys.
     */
    LogData(std::unique_ptr<LogFile> file, std::vector<LogLine> lines, std::vector<std::string> keys);

    // Deleted copy constructor and copy assignment operator for efficiency.
    LogData(LogData &) = delete;
    LogData &operator=(LogData &) = delete;

    // Defaulted move constructor and move assignment operator.
    LogData(LogData &&) = default;
    LogData &operator=(LogData &&) = default;

    /**
     * @brief Get the log files.
     *
     * @return const reference to the vector of unique pointers to LogFile.
     */
    const std::vector<std::unique_ptr<LogFile>> &Files() const;
    std::vector<std::unique_ptr<LogFile>> &Files();

    /**
     * @brief Get the log lines.
     *
     * @return const reference to the vector of LogLine objects.
     */
    const std::vector<LogLine> &Lines() const;
    std::vector<LogLine> &Lines();

    /**
     * @brief Get the log keys.
     *
     * @return const reference to the vector of strings representing log keys.
     */
    const std::vector<std::string> &Keys() const;
    std::vector<std::string> &Keys();

    /**
     * @brief Merge another LogData object into this one.
     *
     * @param other The LogData object to merge into this one.
     */
    void Merge(LogData &&other);

private:
    std::vector<std::unique_ptr<LogFile>> mFiles;
    std::vector<LogLine> mLines;
    std::vector<std::string> mKeys;
};

} // namespace loglib
