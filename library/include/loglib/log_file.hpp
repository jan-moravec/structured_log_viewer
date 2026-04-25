#pragma once

#include <mio/mmap.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
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
     * @brief Updates the line number being referenced.
     *
     * Used by Stage C of the streaming pipeline to back-fill the absolute line
     * numbers onto LogLines that Stage B emitted with a placeholder.
     */
    void SetLineNumber(size_t lineNumber);

    /**
     * @brief Reads and retrieves the content of the referenced log line.
     *
     * @return std::string The content of the referenced log line.
     */
    std::string GetLine() const;

private:
    LogFile *mLogFile = nullptr;
    size_t mLineNumber = 0;
};

/**
 * @brief Represents a log file, owning the memory-mapped backing storage and
 *        the list of byte offsets at which each line starts.
 *
 * The mmap is owned for the entire lifetime of the `LogFile`, which is what
 * allows `LogValue` instances to hold `std::string_view`s into the file content
 * (PRD req. 4.1.6a). Moving a `LogFile` transfers the mmap; the underlying
 * `mio::mmap_source` guarantees that the mapped pointer survives the move
 * (a property pinned by the Catch2 test in `test_log_file.cpp`).
 */
class LogFile
{
public:
    /**
     * @brief Opens @p filePath as a memory-mapped file.
     *
     * Throws `std::runtime_error` if the file cannot be opened or mapped.
     *
     * @param filePath Path to the log file.
     */
    explicit LogFile(std::filesystem::path filePath);

    LogFile(const LogFile &) = delete;
    LogFile &operator=(const LogFile &) = delete;

    LogFile(LogFile &&) noexcept = default;
    LogFile &operator=(LogFile &&) noexcept = default;

    /**
     * @brief Retrieves the path to the log file.
     */
    const std::filesystem::path &GetPath() const;

    /**
     * @brief Returns a pointer to the first byte of the memory-mapped file.
     *
     * The pointer is stable for the entire lifetime of the `LogFile`,
     * including across moves (PRD req. 4.1.6a).
     */
    const char *Data() const;

    /**
     * @brief Returns the size of the memory-mapped file in bytes.
     */
    size_t Size() const;

    /**
     * @brief Reads and retrieves a specific line from the log file.
     *
     * Returns the slice of the mmap that corresponds to the requested line,
     * with any trailing `'\r'` trimmed (Windows CRLF normalisation). Throws
     * `std::out_of_range` if @p lineNumber is past the end.
     */
    std::string GetLine(size_t lineNumber) const;

    /**
     * @brief Gets the total number of lines registered via `CreateReference`
     *        (or appended in batch by the streaming pipeline).
     */
    size_t GetLineCount() const;

    /**
     * @brief Reserves capacity for @p count line offsets in the internal
     *        offset table.
     *
     * Cheap hint to avoid repeated reallocations when the number of lines is
     * roughly known ahead of time (e.g. the streaming pipeline calls this with
     * `file_size / 100` before launching).
     */
    void ReserveLineOffsets(size_t count);

    /**
     * @brief Creates a reference to a specific line in the log file.
     *
     * Records the byte offset of the next line and returns a reference to the
     * line just registered. The returned reference's line number indexes into
     * the order in which references were created.
     *
     * @param position Byte offset of the *next* line in the file (i.e. the
     *                 byte just past the trailing newline of the line being
     *                 registered). Must be strictly greater than the previous
     *                 registered offset.
     */
    LogFileReference CreateReference(size_t position);

    /**
     * @brief Appends a contiguous block of pre-computed line offsets.
     *
     * Used by the streaming Stage C appender to merge a batch's local line
     * offsets into the global offset table in a single `vector::insert` call
     * (PRD req. 4.2.18 Stage C, 4.1.6a). The caller must ensure the offsets
     * are strictly increasing and the first offset is greater than
     * `mLineOffsets.back()`.
     */
    void AppendLineOffsets(const std::vector<uint64_t> &offsets);

private:
    std::filesystem::path mPath;
    mio::mmap_source mMmap;

    /// Byte offsets of every line boundary plus a sentinel one-past-the-last.
    /// Element 0 is always 0 (the start of line 0). For a line @p i, the bytes
    /// are `[mLineOffsets[i], mLineOffsets[i+1])` minus the trailing newline.
    std::vector<uint64_t> mLineOffsets;
};

} // namespace loglib
