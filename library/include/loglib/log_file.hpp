#pragma once

#include <filesystem>
#include <fstream>
#include <iosfwd>
#include <string>
#include <vector>

namespace loglib
{

class LogFile;

/**
 * @brief Represents a reference to a specific line in a log file.
 *
 */
class LogFileReference
{
public:
    /**
     * @brief Constructs a LogFileReference object for a specific line in a log file.
     *
     * @param logFile The LogFile object containing the log data.
     * @param lineNumber The line number being referenced (0-based index).
     */
    LogFileReference(LogFile &logFile, size_t lineNumber);

    /**
     * @brief Retrieves the path to the log file being referenced.
     *
     * @return const std::filesystem::path& Path to the referenced log file.
     */
    const std::filesystem::path &GetPath() const;

    /**
     * @brief Retrieves the line number being referenced.
     *
     * @return size_t The referenced log line number (0-based index).
     */
    size_t GetLineNumber() const;

    /**
     * @brief Reads and retrieves the content of the referenced log line.
     *
     * @return std::string The content of the referenced log line.
     */
    std::string GetLine() const;

private:
    LogFile &mLogFile;
    const size_t mLineNumber = 0;
};

/**
 * @brief Represents a log file, including its path and parsed line offsets.
 *
 */
class LogFile
{
public:
    /**
     * @brief Constructs a LogFile object with the specified file path and line offsets.
     *
     * @param filePath Path to the log file.
     */
    explicit LogFile(std::filesystem::path filePath);

    // Deleted copy constructor and copy assignment operator for efficiency.
    LogFile(LogFile &) = delete;
    LogFile &operator=(LogFile &) = delete;

    // Defaulted move constructor and move assignment operator.
    LogFile(LogFile &&) = default;
    LogFile &operator=(LogFile &&) = default;

    /**
     * @brief Retrieves the path to the log file.
     *
     * @return const std::filesystem::path& Path to the log file.
     */
    const std::filesystem::path &GetPath() const;

    /**
     * @brief Reads and retrieves a specific line from the log file.
     *
     * @param lineNumber The line number to retrieve (0-based index).
     * @return std::string The content of the specified log line.
     */
    std::string GetLine(uint64_t lineNumber);

    /**
     * @brief Gets the total number of lines in the log file.
     *
     * @return size_t The total number of log lines.
     */
    size_t GetLineCount() const;

    /**
     * @brief Creates a reference to a specific line in the log file.
     *
     * @param position Stream position of the line in the log file.
     * @return LogFileReference Reference to the specified log line.
     */
    LogFileReference CreateReference(std::streampos position);
    LogFileReference CreateReference(size_t position);

private:
    const std::filesystem::path mPath;
    std::ifstream mFile;

    // Collection of offsets indicating the start of each log line in the file.
    // Starts from 0 and ends with the last character position in the file.
    std::vector<std::streampos> mLineOffsets;
};

} // namespace loglib
