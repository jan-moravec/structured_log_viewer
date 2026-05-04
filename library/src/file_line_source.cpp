#include "loglib/file_line_source.hpp"

#include "loglib/log_file.hpp"

#include <stdexcept>
#include <utility>

namespace loglib
{

FileLineSource::FileLineSource(std::unique_ptr<LogFile> file) : mOwnedFile(std::move(file))
{
    if (mOwnedFile == nullptr)
    {
        throw std::invalid_argument("FileLineSource: file must not be null");
    }
    mFile = mOwnedFile.get();
}

FileLineSource::~FileLineSource() = default;

const std::filesystem::path &FileLineSource::Path() const noexcept
{
    return mFile->GetPath();
}

std::string FileLineSource::RawLine(size_t lineId) const
{
    return mFile->GetLine(lineId);
}

std::string_view FileLineSource::ResolveMmapBytes(uint64_t offset, uint32_t length, size_t /*lineId*/) const noexcept
{
    const char *data = mFile->Data();
    const size_t size = mFile->Size();
    if (data == nullptr || offset > size || offset + length > size)
    {
        return {};
    }
    return std::string_view(data + offset, length);
}

std::string_view FileLineSource::ResolveOwnedBytes(uint64_t offset, uint32_t length, size_t /*lineId*/) const noexcept
{
    const std::string_view arena = mFile->OwnedStringsView();
    if (offset > arena.size() || offset + length > arena.size())
    {
        return {};
    }
    return arena.substr(offset, length);
}

std::span<const char> FileLineSource::StableBytes() const noexcept
{
    const char *data = mFile->Data();
    const size_t size = mFile->Size();
    if (data == nullptr || size == 0)
    {
        return {};
    }
    return std::span<const char>(data, size);
}

uint64_t FileLineSource::AppendOwnedBytes(size_t /*lineId*/, std::string_view bytes)
{
    // Forward to the session-global arena shared by every line.
    return mFile->AppendOwnedStrings(bytes);
}

bool FileLineSource::SupportsEviction() const noexcept
{
    return false;
}

void FileLineSource::EvictBefore(size_t /*firstSurvivingLineId*/)
{
    // Finite mmap-backed sources never evict.
}

size_t FileLineSource::FirstAvailableLineId() const noexcept
{
    return 0;
}

LogFile &FileLineSource::File() noexcept
{
    return *mFile;
}

const LogFile &FileLineSource::File() const noexcept
{
    return *mFile;
}

std::unique_ptr<LogFile> FileLineSource::ReleaseFile() noexcept
{
    // `mFile` still points at the released file so resolution remains
    // valid; the caller must keep the file alive for our lifetime.
    return std::move(mOwnedFile);
}

} // namespace loglib
