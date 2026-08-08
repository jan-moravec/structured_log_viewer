#include "loglib/stdin_bytes_producer.hpp"

#include "loglib/internal/line_bytes_queue.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

namespace loglib
{

namespace
{

#ifdef _WIN32
using NativeHandle = HANDLE;
const NativeHandle INVALID_NATIVE = nullptr;
#else
using NativeHandle = int;
constexpr NativeHandle INVALID_NATIVE = -1;
#endif

bool IsValidNative(NativeHandle h) noexcept
{
#ifdef _WIN32
    return h != nullptr && h != INVALID_HANDLE_VALUE;
#else
    return h >= 0;
#endif
}

void CloseNative(NativeHandle h) noexcept
{
    if (!IsValidNative(h))
    {
        return;
    }
#ifdef _WIN32
    ::CloseHandle(h);
#else
    ::close(h);
#endif
}

/// Duplicate the process's stdin so `Stop()` can close the copy
/// without affecting the parent process's view of FD 0. Throws
/// `std::runtime_error` on failure.
NativeHandle DuplicateStdin()
{
#ifdef _WIN32
    auto *const stdinHandle = ::GetStdHandle(STD_INPUT_HANDLE);
    if (stdinHandle == INVALID_HANDLE_VALUE || stdinHandle == nullptr)
    {
        throw std::runtime_error("StdinBytesProducer: GetStdHandle(STD_INPUT_HANDLE) failed");
    }
    HANDLE dup = nullptr;
    auto *const currentProc = ::GetCurrentProcess();
    if (::DuplicateHandle(
            currentProc,
            stdinHandle,
            currentProc,
            &dup,
            /*dwDesiredAccess=*/0,
            /*bInheritHandle=*/FALSE,
            DUPLICATE_SAME_ACCESS
        ) == 0)
    {
        throw std::runtime_error("StdinBytesProducer: DuplicateHandle(stdin) failed");
    }
    return dup;
#else
    const int dup = ::dup(STDIN_FILENO);
    if (dup < 0)
    {
        throw std::runtime_error("StdinBytesProducer: dup(STDIN_FILENO) failed");
    }
    return dup;
#endif
}

/// Blocking read from @p handle into @p buffer. Returns bytes
/// read, 0 on clean EOF, or -1 on hard error (peer closed after
/// `Stop()`, invalid handle, etc.). Retries `EINTR` on POSIX.
long long ReadNative(NativeHandle handle, char *buffer, std::size_t size)
{
#ifdef _WIN32
    DWORD got = 0;
    if (::ReadFile(handle, buffer, static_cast<DWORD>(size), &got, nullptr) == 0)
    {
        const DWORD err = ::GetLastError();
        // `Stop()` closed the dup out from under us; treat as EOF.
        if (err == ERROR_BROKEN_PIPE || err == ERROR_INVALID_HANDLE || err == ERROR_OPERATION_ABORTED)
        {
            return 0;
        }
        return -1;
    }
    return static_cast<long long>(got);
#else
    for (;;)
    {
        const ::ssize_t got = ::read(handle, buffer, size);
        if (got >= 0)
        {
            return static_cast<long long>(got);
        }
        if (errno == EINTR)
        {
            continue;
        }
        // `Stop()` closed the dup: `read` returns -1 with EBADF.
        // Treat as EOF for consumers.
        if (errno == EBADF)
        {
            return 0;
        }
        return -1;
    }
#endif
}

#ifndef _WIN32
/// POSIX self-pipe: pair of FDs used to wake the worker's blocking
/// `poll` without closing the input FD out from under a concurrent
/// `read`. Setting both ends `FD_CLOEXEC` avoids leaks across a
/// possible future `fork`+`exec`; setting the write end
/// `O_NONBLOCK` lets `Stop()` post a wake without ever blocking on
/// a full pipe buffer.
///
/// Throws `std::runtime_error` on failure so the ctor never leaves
/// the impl half-initialised.
void MakeSelfPipe(int fds[2])
{
    if (::pipe(fds) != 0)
    {
        throw std::runtime_error("StdinBytesProducer: pipe() (wake) failed");
    }
    const int readFlags = ::fcntl(fds[0], F_GETFD, 0);
    if (readFlags >= 0)
    {
        ::fcntl(fds[0], F_SETFD, readFlags | FD_CLOEXEC);
    }
    const int writeFlags = ::fcntl(fds[1], F_GETFD, 0);
    if (writeFlags >= 0)
    {
        ::fcntl(fds[1], F_SETFD, writeFlags | FD_CLOEXEC);
    }
    const int writeStatus = ::fcntl(fds[1], F_GETFL, 0);
    if (writeStatus >= 0)
    {
        ::fcntl(fds[1], F_SETFL, writeStatus | O_NONBLOCK);
    }
}
#endif

} // namespace

namespace internal
{

class StdinBytesProducerImpl
{
public:
    StdinBytesProducerImpl(NativeHandle handle, StdinBytesProducer::Options options)
        : mHandle(handle), mOptions(std::move(options))
    {
        if (!IsValidNative(mHandle))
        {
            throw std::runtime_error("StdinBytesProducer: invalid native handle");
        }
#ifndef _WIN32
        // Self-pipe wake FDs are set up before the worker starts.
        // `Stop()` writes a byte to `mWakeWrite` to unblock the
        // worker's `poll`; the input FD is only closed once the
        // worker has fully exited, side-stepping the POSIX data
        // race between a concurrent `close(fd)` and `read(fd)`
        // that TSan (correctly) flags on the FD table.
        int wakeFds[2] = {-1, -1};
        MakeSelfPipe(wakeFds);
        mWakeRead = wakeFds[0];
        mWakeWrite = wakeFds[1];
#endif
        mWorker = std::thread([this] { WorkerMain(); });
    }

    ~StdinBytesProducerImpl()
    {
        Stop();
    }

    StdinBytesProducerImpl(const StdinBytesProducerImpl &) = delete;
    StdinBytesProducerImpl &operator=(const StdinBytesProducerImpl &) = delete;
    StdinBytesProducerImpl(StdinBytesProducerImpl &&) = delete;
    StdinBytesProducerImpl &operator=(StdinBytesProducerImpl &&) = delete;

    std::size_t Read(std::span<char> buffer)
    {
        const std::scoped_lock lock(mLock);
        return mQueue.Read(buffer);
    }

    void WaitForBytes(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(mLock);
        mCv.wait_for(lock, timeout, [this] { return !mQueue.Empty() || mClosed.load(std::memory_order_acquire); });
    }

    void Stop() noexcept
    {
        bool expected = false;
        if (!mStopRequested.compare_exchange_strong(expected, true))
        {
            // Another thread already won the CAS and is (or has been)
            // driving the cancel/join sequence. `std::thread::join`
            // itself is not safe from multiple threads -- a second
            // `join()` on the same underlying handle is UB -- so we
            // must NOT touch `mWorker` here. Instead park on the
            // `mStopFinished` latch: the winner sets it under
            // `mStopFinishedLock` after `mWorker.join()` returns.
            //
            // Re-entrant call from the worker thread itself (`Stop()`
            // invoked from within `WorkerMain`, e.g. via a chained
            // owner destructor) never reaches the latch: it would
            // self-deadlock. Return immediately in that case; the
            // winner will observe `mStopFinished` once the worker
            // function actually returns.
            if (std::this_thread::get_id() == mWorker.get_id())
            {
                return;
            }
            std::unique_lock<std::mutex> lock(mStopFinishedLock);
            mStopFinishedCv.wait(lock, [this] { return mStopFinished.load(std::memory_order_acquire); });
            return;
        }

        // Unblock the worker so it can observe `mStopRequested`
        // and return. Publish `mClosed` first so any concurrent
        // `WaitForBytes` observes it on the next wake.
        //
        // Windows: `CloseHandle` while another thread is inside
        // `ReadFile` on that handle is undefined behavior (the
        // reader may hang forever); use `CancelIoEx` to abort
        // the pending IO, join the worker, then close.
        //
        // POSIX: `close(fd)` while another thread is inside
        // `read(fd)` is a documented data race on the FD table
        // (TSan flags it, and the kernel is only guaranteed to
        // unblock the read on modern Linux -- not on macOS, BSD,
        // or older kernels). Wake the worker through a self-pipe
        // instead: it polls `[mHandle, mWakeRead]` between reads,
        // observes the wake byte, drops out of the loop, and
        // returns cleanly; `close(mHandle)` runs only after the
        // worker has joined.
#ifdef _WIN32
        NativeHandle handleSnapshot = INVALID_NATIVE;
#endif
        {
            const std::scoped_lock lock(mLock);
#ifdef _WIN32
            handleSnapshot = mHandle;
#endif
            mClosed.store(true, std::memory_order_release);
        }

#ifdef _WIN32
        if (IsValidNative(handleSnapshot))
        {
            ::CancelIoEx(handleSnapshot, nullptr);
        }
#else
        // Post a wake byte through the self-pipe. Retry on
        // `EINTR`; `EAGAIN` means the wake byte is already
        // buffered and the worker will observe it on its next
        // `poll`. Any other error means the pipe is unusable;
        // fall through -- `mStopRequested` is already set, so
        // the worker will still exit on its next iteration once
        // the current `read` unblocks.
        for (;;)
        {
            const char b = 0;
            const ::ssize_t rc = ::write(mWakeWrite, &b, 1);
            if (rc == 1)
            {
                break;
            }
            if (rc < 0 && errno == EINTR)
            {
                continue;
            }
            break;
        }
#endif
        mCv.notify_all();

#ifdef _WIN32
        // Race guard: `CancelIoEx` above only cancels
        // *currently pending* IO. If the worker snapshotted
        // `mHandle` but has not yet entered `ReadFile` by the
        // time we reached this point, that first cancel is a
        // no-op and the worker's next `ReadFile` would block
        // indefinitely. Loop `WaitForSingleObject` with a short
        // interval and re-cancel on each miss until the worker
        // exits. Both `CancelIoEx` and `CancelSynchronousIo`
        // are idempotent, so re-issuing is safe. `Sleep(0)` /
        // short waits keep the busy loop off a spin.
        if (mWorker.joinable() && std::this_thread::get_id() != mWorker.get_id())
        {
            auto *const threadHandle = mWorker.native_handle();
            constexpr DWORD CANCEL_POLL_INTERVAL_MS = 25;
            while (true)
            {
                const DWORD waitResult = ::WaitForSingleObject(threadHandle, CANCEL_POLL_INTERVAL_MS);
                if (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_FAILED)
                {
                    break;
                }
                if (IsValidNative(handleSnapshot))
                {
                    ::CancelIoEx(handleSnapshot, nullptr);
                }
                // Belt-and-braces: target the worker thread's
                // pending synchronous IO directly. Covers any
                // fresh `ReadFile` issued after the last
                // `CancelIoEx`.
                ::CancelSynchronousIo(threadHandle);
            }
            mWorker.join();
        }
#else
        if (mWorker.joinable() && std::this_thread::get_id() != mWorker.get_id())
        {
            mWorker.join();
        }
#endif

        // Safe to close now: the worker has exited so no in-flight
        // `ReadFile` / `read` remains on this handle.
        {
            const std::scoped_lock lock(mLock);
            const NativeHandle h = mHandle;
            mHandle = INVALID_NATIVE;
            CloseNative(h);
        }
#ifndef _WIN32
        if (mWakeRead >= 0)
        {
            ::close(mWakeRead);
            mWakeRead = -1;
        }
        if (mWakeWrite >= 0)
        {
            ::close(mWakeWrite);
            mWakeWrite = -1;
        }
#endif

        // Release any peer `Stop()` calls that CAS-lost above and are
        // parked on the latch. Publish *after* the join + handle close
        // so a peer's post-return observation sees a fully quiesced
        // producer.
        {
            const std::scoped_lock lock(mStopFinishedLock);
            mStopFinished.store(true, std::memory_order_release);
        }
        mStopFinishedCv.notify_all();
    }

    [[nodiscard]] bool IsClosed() const noexcept
    {
        return mClosed.load(std::memory_order_acquire);
    }

    [[nodiscard]] const std::string &DisplayName() const noexcept
    {
        return mOptions.displayName;
    }

    [[nodiscard]] std::size_t DroppedByteCount() const noexcept
    {
        return mDropped.load(std::memory_order_acquire);
    }

private:
    void WorkerMain()
    {
        std::vector<char> chunk(mOptions.readChunkBytes);
        while (!mStopRequested.load(std::memory_order_acquire))
        {
            NativeHandle handle = INVALID_NATIVE;
            {
                const std::scoped_lock lock(mLock);
                handle = mHandle;
            }
            if (!IsValidNative(handle))
            {
                break;
            }
            // Double-check after snapshotting the handle to close
            // the race with `Stop()`: if Stop set `mStopRequested`
            // + fired `CancelIoEx` before we entered `ReadNative`,
            // the cancel would be a no-op and the blocking read
            // would hang. Stop() also retries the cancel from a
            // wait-loop, but bailing out here avoids the pending
            // syscall entirely.
            if (mStopRequested.load(std::memory_order_acquire))
            {
                break;
            }
#ifndef _WIN32
            // Wait for the input FD or the self-pipe wake FD to
            // become readable. On wake we exit cleanly; the read
            // FD is only ever closed after `join()`, so no
            // concurrent `close(fd)` races the `read(fd)` below.
            struct ::pollfd pfds[2];
            pfds[0].fd = handle;
            pfds[0].events = POLLIN;
            pfds[0].revents = 0;
            pfds[1].fd = mWakeRead;
            pfds[1].events = POLLIN;
            pfds[1].revents = 0;
            const int prc = ::poll(pfds, 2, -1);
            if (prc < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                break;
            }
            if ((pfds[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0)
            {
                break;
            }
            if ((pfds[0].revents & (POLLIN | POLLHUP | POLLERR)) == 0)
            {
                continue;
            }
#endif
            const long long got = ReadNative(handle, chunk.data(), chunk.size());
            if (got > 0)
            {
                const std::scoped_lock lock(mLock);
                mQueue.Append(chunk.data(), static_cast<std::size_t>(got), mOptions.queueCapBytes, mDropped);
                mCv.notify_all();
                continue;
            }
            // got == 0 (EOF) or got == -1 (hard error). Both are
            // terminal for stdin; nothing to recover.
            break;
        }

        {
            const std::scoped_lock lock(mLock);
            mClosed.store(true, std::memory_order_release);
        }
        mCv.notify_all();
    }

    mutable std::mutex mLock;
    std::condition_variable mCv;
    NativeHandle mHandle = INVALID_NATIVE;
#ifndef _WIN32
    // POSIX self-pipe used to wake the worker's `poll` without
    // closing the input FD from a concurrent thread. See `Stop()`
    // for the full rationale.
    int mWakeRead = -1;
    int mWakeWrite = -1;
#endif
    StdinBytesProducer::Options mOptions;
    LineBytesQueue mQueue;
    std::atomic<std::size_t> mDropped{0};
    std::atomic<bool> mClosed{false};
    std::atomic<bool> mStopRequested{false};
    // Signalled by the CAS-winning `Stop()` call after `mWorker.join()`
    // returns (and, on Windows, after the deferred `CloseHandle`).
    // CAS-losing peers park on `mStopFinishedCv` until then instead of
    // touching `mWorker` themselves -- `std::thread::join` is not
    // callable from multiple threads.
    std::mutex mStopFinishedLock;
    std::condition_variable mStopFinishedCv;
    std::atomic<bool> mStopFinished{false};
    std::thread mWorker;
};

} // namespace internal

StdinBytesProducer::StdinBytesProducer()
    : StdinBytesProducer(Options{})
{
}

StdinBytesProducer::StdinBytesProducer(Options options)
    : mImpl(std::make_unique<internal::StdinBytesProducerImpl>(DuplicateStdin(), std::move(options)))
{
}

StdinBytesProducer::StdinBytesProducer(FromRawTag /*tag*/, void *opaqueHandle, Options options)
{
#ifdef _WIN32
    mImpl = std::make_unique<internal::StdinBytesProducerImpl>(static_cast<HANDLE>(opaqueHandle), std::move(options));
#else
    const int fd = static_cast<int>(reinterpret_cast<std::intptr_t>(opaqueHandle));
    mImpl = std::make_unique<internal::StdinBytesProducerImpl>(fd, std::move(options));
#endif
}

StdinBytesProducer::~StdinBytesProducer() = default;

std::size_t StdinBytesProducer::Read(std::span<char> buffer)
{
    return mImpl->Read(buffer);
}

void StdinBytesProducer::WaitForBytes(std::chrono::milliseconds timeout)
{
    mImpl->WaitForBytes(timeout);
}

void StdinBytesProducer::Stop() noexcept
{
    mImpl->Stop();
}

bool StdinBytesProducer::IsClosed() const noexcept
{
    return mImpl->IsClosed();
}

std::string StdinBytesProducer::DisplayName() const
{
    return mImpl->DisplayName();
}

std::size_t StdinBytesProducer::DroppedByteCount() const noexcept
{
    return mImpl->DroppedByteCount();
}

namespace internal
{

std::unique_ptr<StdinBytesProducer> StdinBytesProducerTestAccess::Create(
    void *opaqueHandle, StdinBytesProducer::Options options
)
{
    // Route through the private `FromRawTag` ctor. `make_unique`
    // cannot see the private ctor even from a friend, so we go
    // via `new` and adopt the raw pointer into `unique_ptr`.
    return std::unique_ptr<StdinBytesProducer>(
        new StdinBytesProducer(StdinBytesProducer::FromRawTag{}, opaqueHandle, std::move(options))
    );
}

} // namespace internal

} // namespace loglib
