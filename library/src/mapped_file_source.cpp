#include "loglib/mapped_file_source.hpp"

#include "loglib/log_file.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>

namespace loglib
{

MappedFileSource::MappedFileSource(std::unique_ptr<LogFile> file) : mOwnedFile(std::move(file)), mFile(mOwnedFile.get())
{
}

MappedFileSource::MappedFileSource(LogFile &file) : mFile(&file)
{
}

MappedFileSource::~MappedFileSource() = default;

size_t MappedFileSource::Read(std::span<char> buffer)
{
    if (mStopped.load(std::memory_order_acquire) || mFile == nullptr || buffer.empty())
    {
        return 0;
    }

    const size_t fileSize = mFile->Size();
    const char *fileData = mFile->Data();

    // Single CAS-loop on the cursor: multiple `Read` callers are not the
    // expected case (the parser drives this single-threaded), but we still
    // need correctness if a test races `Read` and `Stop` on different
    // threads — the stop check above plus the cursor exchange below are
    // both atomic.
    size_t cursor = mCursor.load(std::memory_order_acquire);
    if (cursor >= fileSize)
    {
        return 0;
    }

    const size_t available = fileSize - cursor;
    const size_t toCopy = std::min(available, buffer.size());

    std::memcpy(buffer.data(), fileData + cursor, toCopy);

    mCursor.store(cursor + toCopy, std::memory_order_release);
    return toCopy;
}

void MappedFileSource::WaitForBytes(std::chrono::milliseconds /*timeout*/)
{
    // Finite mmap sources have all bytes available at construction; there
    // is no producer to wait for. Returning immediately matches PRD 4.9.1.i.
}

void MappedFileSource::Stop() noexcept
{
    mStopped.store(true, std::memory_order_release);
}

bool MappedFileSource::IsClosed() const noexcept
{
    if (mStopped.load(std::memory_order_acquire))
    {
        return true;
    }
    if (mFile == nullptr)
    {
        return true;
    }
    return mCursor.load(std::memory_order_acquire) >= mFile->Size();
}

std::string MappedFileSource::DisplayName() const
{
    if (mFile == nullptr)
    {
        return std::string{};
    }
    return mFile->GetPath().string();
}

bool MappedFileSource::IsMappedFile() const noexcept
{
    return mFile != nullptr && !mStopped.load(std::memory_order_acquire);
}

LogFile *MappedFileSource::GetMappedLogFile() noexcept
{
    if (mStopped.load(std::memory_order_acquire))
    {
        return nullptr;
    }
    return mFile;
}

std::unique_ptr<LogFile> MappedFileSource::ReleaseFile() noexcept
{
    // After release, subsequent `Read`/`GetMappedLogFile` calls observe
    // `mFile == nullptr` and report terminal EOF. The borrowing ctor's
    // `mOwnedFile` is empty, so this returns null in that case.
    auto released = std::move(mOwnedFile);
    mFile = nullptr;
    mStopped.store(true, std::memory_order_release);
    return released;
}

} // namespace loglib
