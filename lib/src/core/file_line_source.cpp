#include "loglib/core/file_line_source.hpp"

#include "loglib/core/error.hpp"
#include "loglib/core/internal/path_encoding.hpp"
#include "loglib/core/internal/stable_byte_arena.hpp"

#include <fmt/format.h>
#include <mio/mmap.hpp>

#include <cassert>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
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

/** Holds either the memory mapping or the bytes given to FromBytes. The object is never moved after construction. */
struct FileLineSource::Backing
{
    mio::mmap_source mmap;
    std::string bytes;
};

namespace
{

// Hints sequential read access to the OS (POSIX posix_madvise /
// Windows PrefetchVirtualMemory). Best-effort; failures are ignored.
void HintSequential(const mio::mmap_source &mmap)
{
    if (mmap.empty())
    {
        return;
    }

#ifdef _WIN32
    WIN32_MEMORY_RANGE_ENTRY range;
    // Win32 PrefetchVirtualMemory takes non-const void*; mapping is read-only.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    range.VirtualAddress = const_cast<char *>(mmap.data());
    range.NumberOfBytes = mmap.size();
    (void)::PrefetchVirtualMemory(::GetCurrentProcess(), 1, &range, 0);
#elif defined(__unix__) || defined(__APPLE__)
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): POSIX API takes non-const void*; mapping is read-only.
    (void)::posix_madvise(const_cast<char *>(mmap.data()), mmap.size(), POSIX_MADV_SEQUENTIAL);
#else
    (void)mmap;
#endif
}

} // namespace

// MSVC's <filesystem> casts a combined bitmask back to __std_fs_stats_flags;
// clang's analyzer flags the value as out-of-range. False positive in stdlib.
// NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange)
FileLineSource::FileLineSource(const std::filesystem::path &filePath)
    : FileLineSource(filePath, filePath)
{
}

FileLineSource::FileLineSource(std::filesystem::path storagePath, std::filesystem::path logicalPath)
    : mPath(std::move(logicalPath)),
      mStoragePath(std::move(storagePath)),
      mBacking(std::make_unique<Backing>()),
      mOwnedStrings(std::make_unique<internal::StableByteArena>())
{
    if (!std::filesystem::exists(mStoragePath))
    {
        throw IoError(fmt::format("File '{}' does not exist.", internal::PathToUtf8(mStoragePath)));
    }

    // Empty files: skip mmap (some platforms reject zero-byte mappings); the
    // leading offset sentinel still keeps LineCount() == 0 truthful.
    const auto size = std::filesystem::file_size(mStoragePath);
    if (size > 0)
    {
        std::error_code ec;
        // Pass mio a lossless native path encoding.
#ifdef _WIN32
        mBacking->mmap = mio::make_mmap_source(mStoragePath.wstring(), 0, mio::map_entire_file, ec);
#else
        mBacking->mmap = mio::make_mmap_source(internal::PathToUtf8(mStoragePath), 0, mio::map_entire_file, ec);
#endif
        if (ec)
        {
            throw IoError(
                fmt::format("Failed to memory-map file '{}': {}", internal::PathToUtf8(mStoragePath), ec.message())
            );
        }
        HintSequential(mBacking->mmap);
        mData = mBacking->mmap.data();
        mSize = mBacking->mmap.size();
    }

    InitializeOffsets();
}
// NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange)

FileLineSource::FileLineSource(InMemoryTag /*tag*/, std::filesystem::path displayName, std::string bytes)
    : mPath(std::move(displayName)),
      mBacking(std::make_unique<Backing>()),
      mOwnedStrings(std::make_unique<internal::StableByteArena>())
{
    // Assigned in place, so the buffer does not move again, which matters for short strings that live inside the
    // string object.
    mBacking->bytes = std::move(bytes);
    if (!mBacking->bytes.empty())
    {
        mData = mBacking->bytes.data();
        mSize = mBacking->bytes.size();
    }
    InitializeOffsets();
}

std::unique_ptr<FileLineSource> FileLineSource::FromBytes(std::string bytes, std::filesystem::path displayName)
{
    // The constructor is private, so make_unique cannot be used.
    return std::unique_ptr<FileLineSource>(new FileLineSource(InMemoryTag{}, std::move(displayName), std::move(bytes)));
}

FileLineSource::~FileLineSource() = default;

void FileLineSource::InitializeOffsets()
{
    mLineOffsets.push_back(0);
}

const std::filesystem::path &FileLineSource::Path() const noexcept
{
    return mPath;
}

std::string FileLineSource::RawLine(size_t lineId) const
{
    return std::string(RawLineBytes(lineId));
}

void FileLineSource::AppendRawLine(size_t lineId, std::string &out) const
{
    out.append(RawLineBytes(lineId));
}

std::string_view FileLineSource::RawLineBytes(size_t lineId) const
{
    assert(lineId + 1 < mLineOffsets.size() && "RawLine: line past the end");
    if (lineId + 1 >= mLineOffsets.size())
    {
        return {};
    }

    // Multi-line headers use the offset after their final continuation.
    size_t stopLine = lineId + 1;
    if (HasMultiLineRecords())
    {
        const auto it = mMultiLineSpans.find(lineId);
        if (it != mMultiLineSpans.end() && it->second + 1 < mLineOffsets.size())
        {
            stopLine = it->second + 1;
        }
    }

    const uint64_t startOffset = mLineOffsets[lineId];
    const uint64_t stopOffset = mLineOffsets[stopLine];
    if (stopOffset <= startOffset || startOffset >= mSize)
    {
        return {};
    }

    // Stop offset is one byte past the trailing '\n'. Files without a trailing
    // newline use fileSize + 1 as the sentinel; clamp against the size.
    size_t length = stopOffset - startOffset - 1;
    if (startOffset + length > mSize)
    {
        length = mSize - static_cast<size_t>(startOffset);
    }

    std::string_view bytes(mData + startOffset, length);
    if (!bytes.empty() && bytes.back() == '\r')
    {
        bytes.remove_suffix(1);
    }
    return bytes;
}

std::string_view FileLineSource::ResolveMmapBytes(uint64_t offset, uint32_t length, size_t /*lineId*/) const noexcept
{
    if (mData == nullptr || offset > mSize || offset + length > mSize)
    {
        return {};
    }
    return {mData + offset, length};
}

std::string_view FileLineSource::ResolveOwnedBytes(uint64_t offset, uint32_t length, size_t /*lineId*/) const noexcept
{
    return mOwnedStrings->View(offset, length);
}

std::span<const char> FileLineSource::StableBytes() const noexcept
{
    if (mData == nullptr || mSize == 0)
    {
        return {};
    }
    return {mData, mSize};
}

uint64_t FileLineSource::AppendOwnedBytes(size_t /*lineId*/, std::string_view bytes)
{
    // One arena is shared by every line.
    return mOwnedStrings->Append(bytes);
}

bool FileLineSource::SupportsEviction() const noexcept
{
    return false;
}

void FileLineSource::EvictBefore(size_t /*firstSurvivingLineId*/)
{
    // A file source never evicts.
}

size_t FileLineSource::FirstAvailableLineId() const noexcept
{
    return 0;
}

size_t FileLineSource::LineCount() const
{
    return mLineOffsets.size() - 1;
}

void FileLineSource::ReserveLineOffsets(size_t count)
{
    mLineOffsets.reserve(count);
}

size_t FileLineSource::LineOffsetsMemoryBytes() const noexcept
{
    return mLineOffsets.capacity() * sizeof(uint64_t);
}

void FileLineSource::RegisterMultiLineRecord(size_t headerLineId, size_t lastLineId)
{
    if (lastLineId <= headerLineId)
    {
        return; // Single-line records don't need an entry.
    }
    // Both offsets must be registered before the span.
    assert(headerLineId + 1 < mLineOffsets.size());
    assert(lastLineId + 1 < mLineOffsets.size());
    if (headerLineId + 1 >= mLineOffsets.size() || lastLineId + 1 >= mLineOffsets.size())
    {
        return;
    }
    mMultiLineSpans[headerLineId] = lastLineId;
}

bool FileLineSource::TryExtendOwnedString(uint64_t offset, uint64_t length, std::string_view bytes)
{
    return mOwnedStrings->TryExtend(offset, length, bytes);
}

size_t FileLineSource::OwnedStringsMemoryBytes() const noexcept
{
    return mOwnedStrings->CapacityBytes();
}

void FileLineSource::AttachLifetimeAnchor(std::shared_ptr<void> anchor) noexcept
{
    if (mLifetimeAnchor)
    {
        // Keep both anchors alive until after unmapping.
        struct AnchorPair
        {
            std::shared_ptr<void> previous;
            std::shared_ptr<void> next;
        };
        anchor =
            std::make_shared<AnchorPair>(AnchorPair{.previous = std::move(mLifetimeAnchor), .next = std::move(anchor)});
    }
    mLifetimeAnchor = std::move(anchor);
}

void FileLineSource::AppendLineOffsets(const std::vector<uint64_t> &offsets)
{
#ifndef NDEBUG
    // Strictly monotonic; consumers read [offsets[i], offsets[i+1]) slices.
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
