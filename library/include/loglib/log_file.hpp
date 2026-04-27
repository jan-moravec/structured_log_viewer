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

    /// Returns the 0-based line number being referenced.
    size_t GetLineNumber() const;

    /// Overwrites the stored line number. Used by Stage C to back-fill the
    /// absolute line number on lines that Stage B emitted with a placeholder.
    void SetLineNumber(size_t lineNumber);

    /// Adds @p delta to the stored line number. Used by Stage C to convert
    /// per-batch relative line indices into absolute line numbers.
    void ShiftLineNumber(size_t delta) noexcept;

    /// Returns the content of the referenced log line.
    std::string GetLine() const;

private:
    LogFile *mLogFile = nullptr;
    size_t mLineNumber = 0;
};

/// Memory-mapped log file. Owns the mmap for its entire lifetime so
/// `LogValue` instances can hold `string_view`s into the file content. Moving
/// a `LogFile` transfers the mmap and keeps the mapped pointer stable.
class LogFile
{
public:
    /// Opens @p filePath as a memory-mapped file. Throws `std::runtime_error`
    /// if the file cannot be opened or mapped.
    explicit LogFile(std::filesystem::path filePath);

    LogFile(const LogFile &) = delete;
    LogFile &operator=(const LogFile &) = delete;

    LogFile(LogFile &&) noexcept = default;
    LogFile &operator=(LogFile &&) noexcept = default;

    const std::filesystem::path &GetPath() const;

    /// Pointer to the first byte of the mmap. Stable for the lifetime of the
    /// `LogFile`, including across moves.
    const char *Data() const;

    /// Size of the mmap in bytes.
    size_t Size() const;

    /// Returns the bytes of @p lineNumber with any trailing `'\r'` trimmed.
    /// Throws `std::out_of_range` if @p lineNumber is past the end.
    std::string GetLine(size_t lineNumber) const;

    size_t GetLineCount() const;

    /// Reserves capacity for @p count line offsets. Cheap hint when the number
    /// of lines is roughly known.
    void ReserveLineOffsets(size_t count);

    /// Records the byte offset of the next line and returns a reference to the
    /// line just registered.
    /// @param position Byte offset of the *next* line (i.e. past the trailing
    ///                 newline of the line being registered). Must be strictly
    ///                 greater than the previously registered offset.
    LogFileReference CreateReference(size_t position);

    /// Appends a contiguous block of pre-computed line offsets. Caller must
    /// ensure the offsets are strictly increasing and the first offset is
    /// greater than the current last offset.
    void AppendLineOffsets(const std::vector<uint64_t> &offsets);

private:
    std::filesystem::path mPath;
    mio::mmap_source mMmap;

    /// Byte offsets of every line boundary plus a one-past-the-last sentinel.
    /// Element 0 is always 0; for line @p i the bytes are
    /// `[mLineOffsets[i], mLineOffsets[i+1])` minus the trailing newline.
    std::vector<uint64_t> mLineOffsets;
};

} // namespace loglib
