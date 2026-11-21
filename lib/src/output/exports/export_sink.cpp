#include "loglib/output/exports/export_sink.hpp"
#include "loglib/core/error.hpp"
#include "loglib/core/internal/file_busy_retry.hpp"
#include "loglib/core/internal/path_encoding.hpp"

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace loglib::exports
{

namespace
{

/** @brief The system error code for an errno value. A zero value gives an empty code. */
std::error_code ErrnoCode(int err) noexcept
{
    return {err, std::generic_category()};
}

// MSVC deprecates plain fopen; the _wfopen_s overload is
// what keeps Unicode filenames intact on Windows.
std::FILE *OpenForWriteBinary(const std::filesystem::path &path)
{
#ifdef _WIN32
    std::FILE *file = nullptr;
    const errno_t err = _wfopen_s(&file, path.wstring().c_str(), L"wb");
    if (err != 0 || file == nullptr)
    {
        errno = err;
        return nullptr;
    }
    return file;
#else
    return std::fopen(path.string().c_str(), "wb");
#endif
}

std::filesystem::path TempPathFor(const std::filesystem::path &destination)
{
    auto tempPath = destination;
    tempPath += ".tmp";
    return tempPath;
}

struct StreamIoStatus
{
    int err = 0;
    bool streamError = false;
#ifdef _WIN32
    unsigned long winError = 0;
#endif
};

void PrepareStreamIo()
{
    errno = 0;
#ifdef _WIN32
    ::SetLastError(0);
#endif
}

StreamIoStatus CaptureStreamIoStatus(std::FILE *file)
{
    StreamIoStatus status;
    status.err = errno;
#ifdef _WIN32
    status.winError = ::GetLastError();
#endif
    status.streamError = std::ferror(file) != 0;
    return status;
}

// POSIX: a failed fwrite / fflush sets the stream error indicator
// and errno to EPIPE. Windows WriteFile reports
// ERROR_BROKEN_PIPE or ERROR_NO_DATA; the CRT usually also sets
// errno to EPIPE.
bool IsBrokenPipe(const StreamIoStatus &status)
{
#ifdef _WIN32
    if (status.winError == ERROR_BROKEN_PIPE || status.winError == ERROR_NO_DATA)
    {
        return true;
    }
#endif
    return status.streamError && status.err == EPIPE;
}

} // namespace

FileSink::FileSink(std::filesystem::path destination)
    : mDestination(std::move(destination)), mTempPath(TempPathFor(mDestination)), mFile(OpenForWriteBinary(mTempPath))
{
    if (mFile == nullptr)
    {
        throw IoError("Failed to open '" + internal::PathToUtf8(mTempPath) + "' for writing", ErrnoCode(errno));
    }
}

FileSink::~FileSink()
{
    if (mFinished)
    {
        return;
    }
    if (mFile != nullptr)
    {
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory,cert-err33-c)
        (void)std::fclose(mFile);
        mFile = nullptr;
    }
    // Best-effort: an error here just leaves the .tmp on disk,
    // not worth propagating from a destructor.
    std::error_code ec;
    std::filesystem::remove(mTempPath, ec);
}

void FileSink::Write(std::string_view bytes)
{
    assert(mFile != nullptr && "FileSink::Write needs a sink that has not finished or failed");
    if (bytes.empty())
    {
        return;
    }
    const size_t written = std::fwrite(bytes.data(), 1, bytes.size(), mFile);
    if (written != bytes.size())
    {
        throw IoError("Failed to write '" + internal::PathToUtf8(mTempPath) + "'", ErrnoCode(errno));
    }
}

void FileSink::Finish()
{
    if (mFinished)
    {
        return;
    }
    assert(mFile != nullptr && "FileSink::Finish needs a sink that has not failed");
    // Check both: network shares and deferred-write filesystems
    // only surface I/O errors at flush / close time.
    if (std::fflush(mFile) != 0)
    {
        const std::error_code code = ErrnoCode(errno);
        // Already throwing; close is just to release the handle.
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory,cert-err33-c)
        (void)std::fclose(mFile);
        mFile = nullptr;
        throw IoError("Failed to flush '" + internal::PathToUtf8(mTempPath) + "'", code);
    }
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
    if (std::fclose(mFile) != 0)
    {
        const std::error_code code = ErrnoCode(errno);
        mFile = nullptr;
        throw IoError("Failed to close '" + internal::PathToUtf8(mTempPath) + "'", code);
    }
    mFile = nullptr;

    const std::error_code ec = internal::RenameRetryingWhileFileBusy(mTempPath, mDestination);
    if (ec)
    {
        // Rename fails cross-device or when the destination is
        // held open. Fall back to copy + remove for the cross-
        // device case; anything else propagates as a throw.
        if (ec == std::errc::cross_device_link)
        {
            std::error_code copyEc;
            std::filesystem::copy_file(
                mTempPath, mDestination, std::filesystem::copy_options::overwrite_existing, copyEc
            );
            if (copyEc)
            {
                std::error_code cleanupEc;
                std::filesystem::remove(mTempPath, cleanupEc);
                throw IoError(
                    "Failed to move '" + internal::PathToUtf8(mTempPath) + "' to '" +
                        internal::PathToUtf8(mDestination) + "'",
                    copyEc
                );
            }
            std::error_code removeEc;
            std::filesystem::remove(mTempPath, removeEc);
        }
        else
        {
            std::error_code cleanupEc;
            std::filesystem::remove(mTempPath, cleanupEc);
            throw IoError(
                "Failed to rename '" + internal::PathToUtf8(mTempPath) + "' to '" + internal::PathToUtf8(mDestination) +
                    "'",
                ec
            );
        }
    }
    mFinished = true;
}

StreamSink::StreamSink(std::FILE *file)
    : mFile(file)
{
    assert(mFile != nullptr && "StreamSink needs an open FILE");
}

void StreamSink::Write(std::string_view bytes)
{
    assert(!mFinished && "StreamSink::Write needs a sink that has not finished");
    while (!bytes.empty())
    {
        PrepareStreamIo();
        const size_t written = std::fwrite(bytes.data(), 1, bytes.size(), mFile);
        if (written == bytes.size())
        {
            return;
        }
        const StreamIoStatus status = CaptureStreamIoStatus(mFile);
        if (IsBrokenPipe(status))
        {
            throw BrokenPipe();
        }
        if (status.err == EINTR)
        {
            bytes.remove_prefix(written);
            continue;
        }
        if (written > 0 && !status.streamError)
        {
            bytes.remove_prefix(written);
            continue;
        }
        throw IoError("Failed to write to stream", ErrnoCode(status.err));
    }
}

void StreamSink::Finish()
{
    if (mFinished)
    {
        return;
    }
    PrepareStreamIo();
    if (std::fflush(mFile) != 0)
    {
        const StreamIoStatus status = CaptureStreamIoStatus(mFile);
        if (IsBrokenPipe(status))
        {
            throw BrokenPipe();
        }
        throw IoError("Failed to flush stream", ErrnoCode(status.err));
    }
    mFinished = true;
}

} // namespace loglib::exports
