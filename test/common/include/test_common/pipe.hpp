#pragma once

#include <stdexcept>
#include <string_view>

#ifndef _WIN32
#include <cstdint>
#endif

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace test_common
{

/// RAII wrapper around a platform pipe. Owns both ends up front;
/// the read-end can be borrowed (`ReadEnd`) for callers that keep
/// their own reference (`StdinRedirect` for the `StdinPeek`
/// fixture) or transferred outright (`TakeReadEndOpaque`) for
/// callers that need to hand the read-end into another owner
/// (`StdinBytesProducerTestAccess::Create` for the producer
/// fixture). Once transferred, `CloseRead` becomes a no-op so
/// the destructor leaves the transferee's handle alone.
///
/// This is a header-only helper so both call sites can share the
/// same platform-branchy Windows / POSIX code without an extra
/// static library. Setup / write failures throw
/// `std::runtime_error`; Catch2 surfaces the exception message
/// alongside the failing assertion with no extra plumbing.
class Pipe
{
public:
    Pipe()
    {
#ifdef _WIN32
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = FALSE;
        HANDLE readEnd = nullptr;
        HANDLE writeEnd = nullptr;
        const BOOL ok = ::CreatePipe(&readEnd, &writeEnd, &sa, /*nSize=*/0);
        if (ok == 0)
        {
            throw std::runtime_error("test_common::Pipe: CreatePipe failed");
        }
        mRead = readEnd;
        mWrite = writeEnd;
#else
        int fds[2] = {-1, -1};
        const int rc = ::pipe(fds);
        if (rc != 0)
        {
            throw std::runtime_error("test_common::Pipe: pipe() failed");
        }
        mRead = fds[0];
        mWrite = fds[1];
#endif
    }

    // Teardown ignores errors because the test may already have
    // closed both ends manually.
    // NOLINTNEXTLINE(bugprone-exception-escape)
    ~Pipe() noexcept
    {
        CloseWrite();
        CloseRead();
    }

    Pipe(const Pipe &) = delete;
    Pipe &operator=(const Pipe &) = delete;

    /// Return the read-end without transferring ownership. Used
    /// by `StdinRedirect` to install the pipe as FD 0 for the
    /// duration of a test; the pipe still owns the handle and
    /// will close it on teardown.
#ifdef _WIN32
    [[nodiscard]] HANDLE ReadEnd() const noexcept
    {
        return mRead;
    }
#else
    [[nodiscard]] int ReadEnd() const noexcept
    {
        return mRead;
    }
#endif

    /// Transfer the read-end to another owner (usually
    /// `StdinBytesProducerTestAccess::Create`). After the
    /// transfer the pipe drops its own reference so
    /// `CloseRead` becomes a no-op. Opaque `void *` return
    /// keeps the API portable between HANDLE and int fd.
    [[nodiscard]] void *TakeReadEndOpaque() noexcept
    {
#ifdef _WIN32
        HANDLE h = mRead;
        mRead = nullptr;
        return static_cast<void *>(h);
#else
        const int fd = mRead;
        mRead = -1;
        return reinterpret_cast<void *>(static_cast<std::intptr_t>(fd));
#endif
    }

    /// Write @p bytes to the write-end. Throws if the platform
    /// call short-writes or fails; pipes don't short-write for
    /// the small payloads tests use so a partial write signals
    /// a broken fixture, not a byte-count race.
    void Write(std::string_view bytes) const
    {
#ifdef _WIN32
        if (mWrite == nullptr)
        {
            throw std::runtime_error("test_common::Pipe: Write called after CloseWrite");
        }
        DWORD written = 0;
        const BOOL ok =
            ::WriteFile(mWrite, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
        if (ok == 0 || written != bytes.size())
        {
            throw std::runtime_error("test_common::Pipe: WriteFile failed or short-wrote");
        }
#else
        if (mWrite < 0)
        {
            throw std::runtime_error("test_common::Pipe: Write called after CloseWrite");
        }
        const ::ssize_t got = ::write(mWrite, bytes.data(), bytes.size());
        if (got != static_cast<::ssize_t>(bytes.size()))
        {
            throw std::runtime_error("test_common::Pipe: write() failed or short-wrote");
        }
#endif
    }

    /// Close the write-end so consumers observe EOF. Idempotent;
    /// tests that manually close mid-run don't have to guard the
    /// destructor call.
    void CloseWrite() noexcept
    {
#ifdef _WIN32
        if (mWrite != nullptr)
        {
            ::CloseHandle(mWrite);
            mWrite = nullptr;
        }
#else
        if (mWrite >= 0)
        {
            ::close(mWrite);
            mWrite = -1;
        }
#endif
    }

    /// Close the read-end if we still own it. No-op after
    /// `TakeReadEndOpaque` transferred ownership elsewhere.
    void CloseRead() noexcept
    {
#ifdef _WIN32
        if (mRead != nullptr)
        {
            ::CloseHandle(mRead);
            mRead = nullptr;
        }
#else
        if (mRead >= 0)
        {
            ::close(mRead);
            mRead = -1;
        }
#endif
    }

private:
#ifdef _WIN32
    HANDLE mRead = nullptr;
    HANDLE mWrite = nullptr;
#else
    int mRead = -1;
    int mWrite = -1;
#endif
};

} // namespace test_common
