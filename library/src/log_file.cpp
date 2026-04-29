#include "loglib/log_file.hpp"

#include <fmt/format.h>

#include <cassert>
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

/// Hints sequential read access to the OS (POSIX `posix_madvise` /
/// Windows `PrefetchVirtualMemory`). Best-effort; failures are ignored.
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

void LogFileReference::ShiftLineNumber(size_t delta) noexcept
{
    mLineNumber += delta;
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

    // Empty files: skip mmap (some platforms reject zero-byte mappings); the
    // leading offset sentinel below still keeps `GetLineCount() == 0` truthful.
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

    // Stop offset is one byte past the trailing '\n'. Files without a trailing
    // newline use `fileSize + 1` as the sentinel; clamp against the mmap size.
    size_t length = static_cast<size_t>(stopOffset - startOffset - 1);

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
#ifndef NDEBUG
    // Line-offset arrays must stay strictly monotonic — every consumer
    // (`GetLine`, `CreateReference`, the streaming pipeline's per-batch
    // line-number arithmetic) reads adjacent slots as `[offsets[i],
    // offsets[i+1])` byte ranges, and a duplicate or out-of-order entry
    // would produce a zero-length or negative-length slice that aliases
    // the wrong line. The invariant is upheld by both `LogFile`'s own
    // mmap-walk constructor and the parser pipeline's Stage A line
    // boundaries, so a violation here is a programming error in a future
    // streaming source rather than user data — assert it in debug builds.
    if (!offsets.empty())
    {
        if (!mLineOffsets.empty())
        {
            assert(offsets.front() > mLineOffsets.back());
        }
        for (size_t i = 1; i < offsets.size(); ++i)
        {
            assert(offsets[i] > offsets[i - 1]);
        }
    }
#endif
    mLineOffsets.insert(mLineOffsets.end(), offsets.begin(), offsets.end());
}

} // namespace loglib
