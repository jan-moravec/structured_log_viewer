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
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
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
    const HANDLE stdinHandle = ::GetStdHandle(STD_INPUT_HANDLE);
    if (stdinHandle == INVALID_HANDLE_VALUE || stdinHandle == nullptr)
    {
        throw std::runtime_error("StdinBytesProducer: GetStdHandle(STD_INPUT_HANDLE) failed");
    }
    HANDLE dup = nullptr;
    const HANDLE currentProc = ::GetCurrentProcess();
    if (::DuplicateHandle(
            currentProc, stdinHandle, currentProc, &dup, /*dwDesiredAccess=*/0,
            /*bInheritHandle=*/FALSE, DUPLICATE_SAME_ACCESS
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
        std::lock_guard<std::mutex> lock(mLock);
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
            if (mWorker.joinable() && std::this_thread::get_id() != mWorker.get_id())
            {
                mWorker.join();
            }
            return;
        }

        // Unblock a pending `Read` / `ReadFile` on the worker
        // thread. Publish `mClosed` first so any concurrent
        // `WaitForBytes` observes it on the next wake.
        //
        // Windows: `CloseHandle` while another thread is inside
        // `ReadFile` on that handle is undefined behavior (the
        // reader may hang forever); use `CancelIoEx` to abort
        // the pending IO, join the worker, then close.
        //
        // POSIX: `close(fd)` while another thread is inside
        // `read(fd)` returns `EBADF` from `read` on Linux and
        // most modern kernels; we close now, join, and rely on
        // `ReadNative`'s `EBADF` handling.
        NativeHandle handleSnapshot = INVALID_NATIVE;
        {
            std::lock_guard<std::mutex> lock(mLock);
            handleSnapshot = mHandle;
            mClosed.store(true, std::memory_order_release);
        }

#ifdef _WIN32
        if (IsValidNative(handleSnapshot))
        {
            ::CancelIoEx(handleSnapshot, nullptr);
        }
#else
        // Take ownership of the FD before closing so the worker's
        // final `read` sees an invalid FD (EBADF), which is
        // treated as EOF in `ReadNative`.
        {
            std::lock_guard<std::mutex> lock(mLock);
            mHandle = INVALID_NATIVE;
        }
        if (IsValidNative(handleSnapshot))
        {
            CloseNative(handleSnapshot);
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
            const HANDLE threadHandle = mWorker.native_handle();
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

#ifdef _WIN32
        // Safe to close now: the worker has exited so no in-
        // flight `ReadFile` remains on this handle.
        {
            std::lock_guard<std::mutex> lock(mLock);
            const NativeHandle h = mHandle;
            mHandle = INVALID_NATIVE;
            CloseNative(h);
        }
#endif
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
                std::lock_guard<std::mutex> lock(mLock);
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
            const long long got = ReadNative(handle, chunk.data(), chunk.size());
            if (got > 0)
            {
                std::lock_guard<std::mutex> lock(mLock);
                mQueue.Append(chunk.data(), static_cast<std::size_t>(got), mOptions.queueCapBytes, mDropped);
                mCv.notify_all();
                continue;
            }
            // got == 0 (EOF) or got == -1 (hard error). Both are
            // terminal for stdin; nothing to recover.
            break;
        }

        {
            std::lock_guard<std::mutex> lock(mLock);
            mClosed.store(true, std::memory_order_release);
        }
        mCv.notify_all();
    }

    mutable std::mutex mLock;
    std::condition_variable mCv;
    NativeHandle mHandle = INVALID_NATIVE;
    StdinBytesProducer::Options mOptions;
    LineBytesQueue mQueue;
    std::atomic<std::size_t> mDropped{0};
    std::atomic<bool> mClosed{false};
    std::atomic<bool> mStopRequested{false};
    std::thread mWorker;
};

} // namespace internal

StdinBytesProducer::StdinBytesProducer() : StdinBytesProducer(Options{}) {}

StdinBytesProducer::StdinBytesProducer(Options options)
    : mImpl(std::make_unique<internal::StdinBytesProducerImpl>(DuplicateStdin(), std::move(options)))
{
}

StdinBytesProducer::StdinBytesProducer(FromRawTag, void *opaqueHandle, Options options)
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
