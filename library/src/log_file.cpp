#include "loglib/log_file.hpp"

#include <fmt/format.h>

#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// memoryapi.h needs windows.h above it.
#include <memoryapi.h>
#elif defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#endif

namespace loglib
{

namespace
{

/**
 * @brief Hints to the OS that the mmap will be read sequentially front-to-back.
 *
 * - On POSIX, calls `posix_madvise(addr, size, POSIX_MADV_SEQUENTIAL)` so the
 *   kernel can prefetch and drop already-read pages aggressively.
 * - On Windows, calls `PrefetchVirtualMemory` with a single range descriptor so
 *   the working-set manager starts the read-ahead before the parser touches
 *   the bytes.
 *
 * Failures are non-fatal — the parser still works without the hint. We do not
 * log the error to avoid noisy output during routine open-file operations.
 */
void HintSequential(const mio::mmap_source &mmap)
{
    if (mmap.size() == 0)
    {
        return;
    }

#if defined(_WIN32)
    WIN32_MEMORY_RANGE_ENTRY range;
    range.VirtualAddress = const_cast<char *>(mmap.data());
    range.NumberOfBytes = mmap.size();
    (void)::PrefetchVirtualMemory(::GetCurrentProcess(), 1, &range, 0);
#elif defined(__unix__) || defined(__APPLE__)
    (void)::posix_madvise(const_cast<char *>(mmap.data()), mmap.size(), POSIX_MADV_SEQUENTIAL);
#else
    (void)mmap;
#endif
}

} // namespace

LogFileReference::LogFileReference(LogFile &logFile, size_t lineNumber) : mLogFile(&logFile), mLineNumber(lineNumber)
{
}

const std::filesystem::path &LogFileReference::GetPath() const
{
    return mLogFile->GetPath();
}

size_t LogFileReference::GetLineNumber() const
{
    return mLineNumber;
}

void LogFileReference::SetLineNumber(size_t lineNumber)
{
    mLineNumber = lineNumber;
}

std::string LogFileReference::GetLine() const
{
    return mLogFile->GetLine(mLineNumber);
}

LogFile::LogFile(std::filesystem::path filePath) : mPath(std::move(filePath))
{
    if (!std::filesystem::exists(mPath))
    {
        throw std::runtime_error(fmt::format("File '{}' does not exist.", mPath.string()));
    }

    // Empty files are tolerated to keep the existing "validate empty file" test path: the mmap
    // ctor would fail on a zero-byte file on some platforms, so short-circuit here. mLineOffsets
    // still gets the leading sentinel so `GetLineCount() == 0` is reported consistently.
    const auto size = std::filesystem::file_size(mPath);
    if (size > 0)
    {
        std::error_code ec;
        mMmap = mio::make_mmap_source(mPath.string(), 0, mio::map_entire_file, ec);
        if (ec)
        {
            throw std::runtime_error(fmt::format("Failed to memory-map file '{}': {}", mPath.string(), ec.message()));
        }
        HintSequential(mMmap);
    }

    mLineOffsets.push_back(0);
}

const std::filesystem::path &LogFile::GetPath() const
{
    return mPath;
}

const char *LogFile::Data() const
{
    return mMmap.data();
}

size_t LogFile::Size() const
{
    return mMmap.size();
}

std::string LogFile::GetLine(size_t lineNumber) const
{
    if (lineNumber + 1 >= mLineOffsets.size())
    {
        throw std::out_of_range("Line number out of range: " + std::to_string(lineNumber));
    }

    const uint64_t startOffset = mLineOffsets[lineNumber];
    const uint64_t stopOffset = mLineOffsets[lineNumber + 1];
    if (stopOffset <= startOffset)
    {
        return std::string{};
    }

    // The stored stop offset is one byte past the trailing '\n'; subtract 1 for the newline.
    // Final line without a trailing newline still works because CreateReference stores
    // `fileSize + 1` as the sentinel for that case (see TestLogFile::CreateLogFile in the
    // test helpers and JsonParser::Parse).
    size_t length = static_cast<size_t>(stopOffset - startOffset - 1);

    // Clamp against the actual mmap size in case the line table includes the sentinel
    // `fileSize + 1` for a file without a trailing newline.
    const size_t mmapSize = mMmap.size();
    if (startOffset + length > mmapSize)
    {
        length = mmapSize - static_cast<size_t>(startOffset);
    }

    std::string buffer(mMmap.data() + startOffset, length);
    if (!buffer.empty() && buffer.back() == '\r')
    {
        buffer.pop_back();
    }
    return buffer;
}

size_t LogFile::GetLineCount() const
{
    return mLineOffsets.size() - 1;
}

void LogFile::ReserveLineOffsets(size_t count)
{
    mLineOffsets.reserve(count);
}

LogFileReference LogFile::CreateReference(size_t position)
{
    if (position <= mLineOffsets.back())
    {
        throw std::runtime_error("Invalid position to create reference: " + std::to_string(position));
    }
    LogFileReference reference(*this, mLineOffsets.size() - 1);
    mLineOffsets.push_back(position);
    return reference;
}

void LogFile::AppendLineOffsets(const std::vector<uint64_t> &offsets)
{
    mLineOffsets.insert(mLineOffsets.end(), offsets.begin(), offsets.end());
}

} // namespace loglib
