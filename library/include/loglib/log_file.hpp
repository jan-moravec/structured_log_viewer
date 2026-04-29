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

/// Reference to a specific line in a `LogFile`.
class LogFileReference
{
public:
    LogFileReference(LogFile &logFile, size_t lineNumber);

    const std::filesystem::path &GetPath() const;
    size_t GetLineNumber() const;

    void SetLineNumber(size_t lineNumber);

    /// Adds @p delta to the stored line number (Stage C: relative -> absolute).
    void ShiftLineNumber(size_t delta) noexcept;

    std::string GetLine() const;

private:
    LogFile *mLogFile = nullptr;
    size_t mLineNumber = 0;
};

/// Memory-mapped log file. Owns the mmap for its lifetime so `LogValue`
/// instances can hold `string_view`s into the file content. Move keeps the
/// mapped pointer stable.
class LogFile
{
public:
    /// Throws `std::runtime_error` if the file cannot be opened or mapped.
    explicit LogFile(std::filesystem::path filePath);

    LogFile(const LogFile &) = delete;
    LogFile &operator=(const LogFile &) = delete;

    LogFile(LogFile &&) noexcept = default;
    LogFile &operator=(LogFile &&) noexcept = default;

    const std::filesystem::path &GetPath() const;
    const char *Data() const;
    size_t Size() const;

    /// Trailing `'\r'` is trimmed. Throws `std::out_of_range` when out of range.
    std::string GetLine(size_t lineNumber) const;
    size_t GetLineCount() const;

    void ReserveLineOffsets(size_t count);

    /// @param position Byte offset of the *next* line. Must be strictly
    ///                 greater than the previously registered offset.
    LogFileReference CreateReference(size_t position);

    /// Caller must ensure offsets are strictly increasing and start past the
    /// current last offset.
    void AppendLineOffsets(const std::vector<uint64_t> &offsets);

private:
    std::filesystem::path mPath;
    mio::mmap_source mMmap;

    /// Byte offsets of every line boundary plus a one-past-the-last sentinel.
    std::vector<uint64_t> mLineOffsets;
};

} // namespace loglib
