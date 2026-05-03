#include "loglib/tailing_bytes_producer.hpp"

#include "loglib/internal/file_identity.hpp"

#include <efsw/efsw.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace
{
// `ssize_t` is POSIX-only; alias to a portable signed type so the I/O
// helpers below have a single signature on all platforms.
using ssize_type = std::int64_t;
} // namespace

namespace loglib
{

namespace
{

#if defined(_WIN32)
// `INVALID_HANDLE_VALUE` is a `reinterpret_cast<HANDLE>((LONG_PTR)-1)`, which
// is not a constant expression in MSVC's C++23 mode (C2131). Fall back to a
// plain `const` global; the alias is internal-linkage either way.
const detail::NativeFileHandle kInvalidHandle = INVALID_HANDLE_VALUE;
#else
constexpr detail::NativeFileHandle kInvalidHandle = -1;
#endif

bool IsValidHandle(detail::NativeFileHandle handle) noexcept
{
#if defined(_WIN32)
    return handle != INVALID_HANDLE_VALUE && handle != nullptr;
#else
    return handle >= 0;
#endif
}

void CloseNativeHandle(detail::NativeFileHandle handle) noexcept
{
    if (!IsValidHandle(handle))
    {
        return;
    }
#if defined(_WIN32)
    ::CloseHandle(handle);
#else
    ::close(handle);
#endif
}

/// Canonicalize @p path so the efsw watcher is always installed on an
/// absolute parent directory (PRD 4.8.5 — "parent-directory granularity
/// on Linux inotify / macOS FSEvents"). `weakly_canonical` handles the
/// edge cases the original `parent_path().empty() → current_path()`
/// fallback missed: relative paths (e.g. `"app.log"`), paths with `..`
/// components, and Windows root-relative paths like `"C:foo.log"` that
/// have an empty `parent_path()` on some `std::filesystem`
/// implementations.
///
/// Falls back to the untouched input on error (bad symlink loop, out-of-
/// memory, filesystem rejection). The fallback keeps the belt-and-braces
/// behaviour so a pathological input still gets a best-effort watch
/// attempt; the `parent_path().empty() → current_path()` branch in the
/// ctor handles that remaining path.
std::filesystem::path CanonicalizeWatchedPath(std::filesystem::path path)
{
    std::error_code ec;
    std::filesystem::path canon = std::filesystem::weakly_canonical(path, ec);
    if (ec || canon.empty())
    {
        return path;
    }
    return canon;
}

/// Open @p path for read with maximum producer-friendly share flags. Returns
/// `kInvalidHandle` on failure (incl. `ENOENT` / `ERROR_FILE_NOT_FOUND`).
detail::NativeFileHandle OpenForTail(const std::filesystem::path &path) noexcept
{
#if defined(_WIN32)
    // FILE_SHARE_DELETE is critical: without it, the producer cannot rename
    // or unlink the file (PRD §7 *No mmap on the tail*). FILE_FLAG_SEQUENTIAL_SCAN
    // hints the cache manager to read ahead.
    return ::CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr
    );
#else
    return ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
#endif
}

/// Sample the open handle's current size in bytes. Returns `std::nullopt`
/// if the OS call failed (typical when the handle has been closed or the
/// underlying inode is gone).
std::optional<uint64_t> SizeOfOpenHandle(detail::NativeFileHandle handle) noexcept
{
    if (!IsValidHandle(handle))
    {
        return std::nullopt;
    }
#if defined(_WIN32)
    LARGE_INTEGER li{};
    if (!::GetFileSizeEx(handle, &li))
    {
        return std::nullopt;
    }
    return static_cast<uint64_t>(li.QuadPart);
#else
    struct stat st{};
    if (::fstat(handle, &st) != 0)
    {
        return std::nullopt;
    }
    return static_cast<uint64_t>(st.st_size);
#endif
}

/// Read up to @p size bytes from @p offset on @p handle into @p out. Does
/// not advance any persistent file pointer (uses `pread` / overlapped
/// `ReadFile`). Returns the number of bytes actually read; a short read
/// is normal at EOF. Returns -1 on hard error.
ssize_type ReadAtOffset(detail::NativeFileHandle handle, void *out, size_t size, uint64_t offset) noexcept
{
    if (!IsValidHandle(handle) || size == 0)
    {
        return 0;
    }
#if defined(_WIN32)
    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFu);
    ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD bytesRead = 0;
    const DWORD toRead = static_cast<DWORD>(std::min<size_t>(size, std::numeric_limits<DWORD>::max()));
    if (!::ReadFile(handle, out, toRead, &bytesRead, &ov))
    {
        const DWORD err = ::GetLastError();
        if (err == ERROR_HANDLE_EOF)
        {
            return 0;
        }
        return -1;
    }
    return static_cast<ssize_type>(bytesRead);
#else
    ssize_type total = 0;
    char *outChars = static_cast<char *>(out);
    while (total < static_cast<ssize_type>(size))
    {
        const ssize_t n = ::pread(handle, outChars + total, size - static_cast<size_t>(total), offset + total);
        if (n == 0)
        {
            break; // EOF
        }
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return -1;
        }
        total += static_cast<ssize_type>(n);
    }
    return total;
#endif
}

} // namespace

namespace detail
{

class TailingBytesProducerImpl;

/// efsw listener glue. Filters parent-directory events down to the
/// basename of our open file and forwards to the source.
class TailingBytesProducerWatcherListener final : public efsw::FileWatchListener
{
public:
    TailingBytesProducerWatcherListener(TailingBytesProducerImpl *impl, std::string filename) noexcept
        : mImpl(impl), mFilename(std::move(filename))
    {
    }

    void handleFileAction(
        efsw::WatchID /*watchid*/,
        const std::string & /*dir*/,
        const std::string &filename,
        efsw::Action /*action*/,
        const std::string &oldFilename = ""
    ) override;

private:
    TailingBytesProducerImpl *mImpl;
    std::string mFilename;
};

/// Pimpl for `TailingBytesProducer`. Owns the worker thread, the mutex /
/// cv pair guarding the byte queue, and the efsw watcher.
class TailingBytesProducerImpl
{
public:
    TailingBytesProducerImpl(std::filesystem::path path, size_t retentionLines, TailingBytesProducer::Options options);

    ~TailingBytesProducerImpl();

    TailingBytesProducerImpl(const TailingBytesProducerImpl &) = delete;
    TailingBytesProducerImpl &operator=(const TailingBytesProducerImpl &) = delete;
    TailingBytesProducerImpl(TailingBytesProducerImpl &&) = delete;
    TailingBytesProducerImpl &operator=(TailingBytesProducerImpl &&) = delete;

    size_t Read(std::span<char> buffer);
    void WaitForBytes(std::chrono::milliseconds timeout);
    void Stop() noexcept;
    [[nodiscard]] bool IsClosed() const noexcept;
    [[nodiscard]] std::string DisplayName() const;
    void SetRotationCallback(std::function<void()> callback);
    void SetStatusCallback(std::function<void(SourceStatus)> callback);
    [[nodiscard]] size_t RotationCount() const noexcept;

    /// Called from the efsw thread. Wakes the worker.
    void OnWatcherEvent() noexcept;

private:
    void WorkerMain();
    void Prefill();
    /// Worker-thread-only. Returns true if a rotation branch matched and
    /// recovery completed (caller should treat next iteration fresh).
    bool DetectAndRecoverRotation();
    /// Worker-thread-only. Reads up to `options.readChunkBytes` of new
    /// bytes from the open handle, splits at `\n`, and pushes complete
    /// lines into `mReadyBuffer`. Returns the number of bytes read.
    size_t ReadMoreBytes();
    /// Worker-thread-only. Closes the active handle (if any).
    void CloseHandle() noexcept;
    /// Caller must hold `mMutex`.
    void AppendBytesAndSplitLocked(const char *data, size_t size);
    /// Caller must hold `mMutex`. Appends `mPartialLine` to ready
    /// (with a synthetic `\n` if not already present) and clears it.
    void FlushPartialLineLocked();
    /// Caller must hold `mMutex`. Compacts `mReadyBuffer` after
    /// substantial consumption.
    void CompactReadyBufferIfNeededLocked();
    /// Fires the rotation callback if 1s has elapsed since the last fire.
    /// PRD 4.8.9 — multi-rotation debounce.
    void FireRotationCallbackIfDebounced();

    /// Fires the status callback if @p status differs from the last
    /// status we reported. Edge-triggered so a poll tick in the same
    /// state is silent (PRD 4.8.8). Caller must NOT hold `mMutex`.
    void FireStatusCallbackIfChanged(SourceStatus status);

    std::filesystem::path mPath;
    std::string mFileName; // basename used for filtering watcher events
    std::string mDisplayName;
    size_t mRetentionLines;
    TailingBytesProducer::Options mOptions;

    mutable std::mutex mMutex;
    std::condition_variable mCv;

    // Protected by mMutex:
    std::vector<char> mReadyBuffer;
    size_t mReadyConsumed = 0;
    std::string mPartialLine;
    bool mWatcherEventPending = false;
    bool mWaiting = false; // true in branch (ii) "path missing" state

    std::atomic<bool> mStopRequested{false};
    std::atomic<bool> mWorkerExited{false};
    std::atomic<size_t> mRotationCount{0};

    // Rotation + status callbacks share one mutex so we can call user
    // code without holding the busy `mMutex`.
    mutable std::mutex mCallbackMutex;
    std::function<void()> mRotationCallback;
    std::function<void(SourceStatus)> mStatusCallback;
    std::chrono::steady_clock::time_point mLastRotationFireTime{};
    SourceStatus mLastReportedStatus = SourceStatus::Running;

    // Worker-thread-only state (no lock needed):
    NativeFileHandle mHandle = kInvalidHandle;
    FileIdentity mIdentityAtOpen;
    uint64_t mReadOffset = 0;
    bool mRotationInProgress = false;

    // efsw integration. Declared here so worker can reference, destroyed
    // from the dtor (after worker join) so the listener can never fire on
    // a half-destroyed impl.
    std::unique_ptr<TailingBytesProducerWatcherListener> mWatcherListener;
    std::unique_ptr<efsw::FileWatcher> mWatcher;
    efsw::WatchID mWatchID = 0;

    std::thread mWorker;
};

void TailingBytesProducerWatcherListener::handleFileAction(
    efsw::WatchID /*watchid*/,
    const std::string & /*dir*/,
    const std::string &filename,
    efsw::Action /*action*/,
    const std::string &oldFilename
)
{
    if (mImpl == nullptr)
    {
        return;
    }
    if (filename == mFilename || oldFilename == mFilename)
    {
        mImpl->OnWatcherEvent();
    }
}

TailingBytesProducerImpl::TailingBytesProducerImpl(
    std::filesystem::path path, size_t retentionLines, TailingBytesProducer::Options options
)
    // Canonicalize up front so every downstream computation — basename,
    // display name, watcher parent — sees an absolute path. Without this
    // the filesystem watcher silently no-ops on plain `"app.log"` inputs
    // (empty `parent_path()`, CWD-fallback brittle under `chdir`).
    : mPath(CanonicalizeWatchedPath(std::move(path))), mFileName(mPath.filename().string()),
      mDisplayName(mPath.string()), mRetentionLines(std::max<size_t>(retentionLines, 1)), mOptions(options)
{
    // Open the file up front. Failure here propagates as an exception
    // (PRD 4.1.7 — open errors surface to the caller, leaving the previous
    // session intact). Transient ENOENT during tailing is handled by the
    // rotation branch (ii), not here.
    mHandle = OpenForTail(mPath);
    if (!IsValidHandle(mHandle))
    {
        const std::error_code ec(errno, std::generic_category());
        throw std::system_error(ec, "TailingBytesProducer: failed to open '" + mDisplayName + "'");
    }
    mIdentityAtOpen = FromOpenHandle(mHandle);

    // Pre-fill used to run here synchronously, but a multi-GB file can
    // block the caller for seconds of backwards-newline scanning
    // (§B.4 of the PR review). The worker thread now runs `Prefill()`
    // in its prologue so the GUI thread only pays the ctor's
    // `OpenForTail` + `efsw` setup cost. Open failures still throw
    // synchronously from this ctor (PRD 4.1.7), and `Read` returns 0
    // until pre-fill lands bytes in the queue — the parser parks on
    // `WaitForBytes` per the existing contract.

    // Install the filesystem watcher unless the test harness has disabled
    // it. We watch the parent directory because Linux `inotify` and macOS
    // `FSEvents` work at directory granularity (PRD 4.8.5); the listener
    // filters down to our basename. The ctor has already canonicalized
    // `mPath`, so `parent_path()` is non-empty on any well-formed input;
    // the `current_path()` fallback below is kept as a belt-and-braces
    // second line of defence for exotic inputs where `weakly_canonical`
    // declined (symlink loop, out-of-memory, FS rejection).
    if (!mOptions.disableNativeWatcher)
    {
        try
        {
            mWatcher = std::make_unique<efsw::FileWatcher>();
            mWatcherListener = std::make_unique<TailingBytesProducerWatcherListener>(this, mFileName);

            std::filesystem::path parent = mPath.parent_path();
            if (parent.empty())
            {
                parent = std::filesystem::current_path();
            }

            mWatchID = mWatcher->addWatch(parent.string(), mWatcherListener.get(), false);
            if (mWatchID > 0)
            {
                mWatcher->watch();
            }
            // efsw signals failure via a negative WatchID; we silently
            // fall back to the polling loop in that case (the worker still
            // works, just at the polling cadence).
        }
        catch (...)
        {
            // efsw construction can throw on resource exhaustion; the
            // polling loop is a graceful fallback. Reset the partials so
            // we don't leak listener pointers into a defunct watcher.
            mWatcher.reset();
            mWatcherListener.reset();
        }
    }

    mWorker = std::thread([this] { WorkerMain(); });
}

TailingBytesProducerImpl::~TailingBytesProducerImpl()
{
    Stop();
    if (mWorker.joinable())
    {
        mWorker.join();
    }
    // Tear down the watcher *after* the worker has exited so the listener
    // never fires on a partially-destroyed impl. efsw's destructor stops
    // the watcher's internal thread.
    if (mWatcher && mWatchID > 0)
    {
        mWatcher->removeWatch(mWatchID);
    }
    mWatcher.reset();
    mWatcherListener.reset();
    CloseHandle();
}

size_t TailingBytesProducerImpl::Read(std::span<char> buffer)
{
    if (buffer.empty())
    {
        return 0;
    }

    std::lock_guard<std::mutex> lock(mMutex);
    const size_t available = mReadyBuffer.size() - mReadyConsumed;
    if (available == 0)
    {
        return 0;
    }
    const size_t toCopy = std::min(available, buffer.size());
    std::memcpy(buffer.data(), mReadyBuffer.data() + mReadyConsumed, toCopy);
    mReadyConsumed += toCopy;
    CompactReadyBufferIfNeededLocked();
    return toCopy;
}

void TailingBytesProducerImpl::WaitForBytes(std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(mMutex);
    const auto predicate = [&] {
        return (mReadyBuffer.size() - mReadyConsumed) > 0 || mStopRequested.load(std::memory_order_acquire) ||
               mWorkerExited.load(std::memory_order_acquire);
    };
    if (timeout.count() <= 0)
    {
        // Non-blocking variant: just consult the predicate.
        return;
    }
    mCv.wait_for(lock, timeout, predicate);
}

void TailingBytesProducerImpl::Stop() noexcept
{
    if (mStopRequested.exchange(true, std::memory_order_acq_rel))
    {
        return; // already stopped
    }
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mWatcherEventPending = true;
    }
    mCv.notify_all();
}

bool TailingBytesProducerImpl::IsClosed() const noexcept
{
    if (!mWorkerExited.load(std::memory_order_acquire))
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(mMutex);
    return (mReadyBuffer.size() - mReadyConsumed) == 0;
}

std::string TailingBytesProducerImpl::DisplayName() const
{
    return mDisplayName;
}

void TailingBytesProducerImpl::SetRotationCallback(std::function<void()> callback)
{
    std::lock_guard<std::mutex> lock(mCallbackMutex);
    mRotationCallback = std::move(callback);
}

void TailingBytesProducerImpl::SetStatusCallback(std::function<void(SourceStatus)> callback)
{
    std::lock_guard<std::mutex> lock(mCallbackMutex);
    mStatusCallback = std::move(callback);
}

size_t TailingBytesProducerImpl::RotationCount() const noexcept
{
    return mRotationCount.load(std::memory_order_acquire);
}

void TailingBytesProducerImpl::OnWatcherEvent() noexcept
{
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mWatcherEventPending = true;
    }
    mCv.notify_all();
}

void TailingBytesProducerImpl::WorkerMain()
{
    // Pre-fill runs here rather than in the ctor so the GUI thread is
    // never blocked on a multi-GB backwards-newline scan (§B.4 of the
    // PR review / PRD 4.1.5–6). The stop check up front ensures a
    // fast `Stop` immediately after construction exits cleanly without
    // doing the work — symmetric with the body loop's stop checks.
    if (!mStopRequested.load(std::memory_order_acquire))
    {
        Prefill();
    }

    while (!mStopRequested.load(std::memory_order_acquire))
    {
        bool didWork = false;

        // 1. Detect / recover rotation. The detect step also handles the
        // "waiting" state for branch (ii) on each tick.
        if (DetectAndRecoverRotation())
        {
            didWork = true;
        }

        // 2. Read newly-available bytes from the current open handle.
        if (IsValidHandle(mHandle))
        {
            const size_t n = ReadMoreBytes();
            if (n > 0)
            {
                didWork = true;
            }
        }

        if (mStopRequested.load(std::memory_order_acquire))
        {
            break;
        }

        if (!didWork)
        {
            // Park on cv until: a watcher event fires, Stop is requested,
            // or the polling fallback fires.
            std::unique_lock<std::mutex> lock(mMutex);
            mCv.wait_for(lock, mOptions.pollInterval, [&] {
                return mWatcherEventPending || mStopRequested.load(std::memory_order_acquire);
            });
            mWatcherEventPending = false;
        }
        else
        {
            // After processing work, clear the pending flag so the next
            // iteration's wait re-arms cleanly.
            std::lock_guard<std::mutex> lock(mMutex);
            mWatcherEventPending = false;
        }
    }

    // Stop has been requested. Flush the partial-line buffer as a final
    // synthetic line unless we are mid-rotation (PRD 4.7.2.ii).
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (!mRotationInProgress)
        {
            FlushPartialLineLocked();
        }
        else
        {
            mPartialLine.clear();
        }
    }

    CloseHandle();
    mWorkerExited.store(true, std::memory_order_release);
    mCv.notify_all();
}

void TailingBytesProducerImpl::Prefill()
{
    const auto sizeOpt = SizeOfOpenHandle(mHandle);
    if (!sizeOpt.has_value() || *sizeOpt == 0)
    {
        mReadOffset = 0;
        return;
    }
    const uint64_t fileSize = *sizeOpt;

    // Walk backwards in `prefillChunkBytes` slices, counting newlines until
    // we have N+1 (so the byte after the (N+1)-th newline from the end is
    // the start of the last N complete lines). PRD 4.1.5-6.
    const size_t targetNewlines = mRetentionLines + 1;
    uint64_t pos = fileSize;
    size_t newlinesSeen = 0;
    uint64_t lineStart = 0; // default: keep the whole file
    bool boundaryFound = false;
    std::vector<char> chunk;
    chunk.resize(mOptions.prefillChunkBytes);

    // PRD §7 *Line buffering*: bound the pre-fill scan so a file with
    // no newlines (or a pathological retentionLines on a multi-GB file)
    // cannot wedge the ctor / the worker for an unbounded amount of
    // time. On overflow we seek to EOF and return no lines — the
    // producer's future writes will still appear in the tail.
    uint64_t scannedBytes = 0;

    while (pos > 0 && !boundaryFound)
    {
        const uint64_t chunkSize = std::min<uint64_t>(pos, mOptions.prefillChunkBytes);
        pos -= chunkSize;
        const ssize_type n = ReadAtOffset(mHandle, chunk.data(), static_cast<size_t>(chunkSize), pos);
        if (n <= 0)
        {
            // Unable to read; abandon prefill and start tail at file_size.
            mReadOffset = fileSize;
            return;
        }
        scannedBytes += static_cast<uint64_t>(n);
        // Walk backwards through the chunk.
        for (size_t i = static_cast<size_t>(n); i > 0; --i)
        {
            if (chunk[i - 1] == '\n')
            {
                ++newlinesSeen;
                if (newlinesSeen == targetNewlines)
                {
                    lineStart = pos + i; // first byte AFTER the newline
                    boundaryFound = true;
                    break;
                }
            }
        }
        if (!boundaryFound && mOptions.prefillMaxScanBytes != 0 && scannedBytes >= mOptions.prefillMaxScanBytes)
        {
            // Scan budget exhausted without finding enough newlines.
            // Yield no lines; tailing resumes from EOF.
            mReadOffset = fileSize;
            return;
        }
    }

    // Read [lineStart, fileSize) and split.
    if (lineStart < fileSize)
    {
        const uint64_t toRead = fileSize - lineStart;
        std::vector<char> body(static_cast<size_t>(toRead));
        const ssize_type n = ReadAtOffset(mHandle, body.data(), body.size(), lineStart);
        if (n > 0)
        {
            std::lock_guard<std::mutex> lock(mMutex);
            AppendBytesAndSplitLocked(body.data(), static_cast<size_t>(n));
        }
    }

    // The tailing read continues from the end of the file as observed at
    // pre-fill time. Bytes [lineStart, fileSize) have been processed; the
    // partial-line buffer (set by AppendBytesAndSplitLocked) seeds the
    // tail's partial-line state per PRD 4.1.5.i.
    mReadOffset = fileSize;

    // Wake any consumer parked on `WaitForBytes`. Without this notify,
    // a parser that parked before pre-fill bytes landed in `mReadyBuffer`
    // would sleep until `mOptions.pollInterval` (250 ms in production)
    // expires — a measurable startup latency hit on large pre-filled
    // files. Symmetric with `ReadMoreBytes`.
    mCv.notify_all();
}

bool TailingBytesProducerImpl::DetectAndRecoverRotation()
{
    const FileIdentity pathId = FromPath(mPath);
    const FileIdentity handleId = FromOpenHandle(mHandle);

    // Branch (i): file-identity changed (rename-and-create or
    // delete-then-recreate that completed between two polls).
    if (pathId.valid && handleId.valid && pathId != handleId)
    {
        mRotationInProgress = true;

        // Recovery: close old handle, open new path, seek to 0.
        CloseHandle();
        mHandle = OpenForTail(mPath);
        mIdentityAtOpen = FromOpenHandle(mHandle);
        mReadOffset = 0;
        const bool wasWaiting = mWaiting;
        mWaiting = false;

        {
            std::lock_guard<std::mutex> lock(mMutex);
            mPartialLine.clear(); // PRD 4.8.7.i
        }

        mRotationCount.fetch_add(1, std::memory_order_acq_rel);
        FireRotationCallbackIfDebounced();
        if (wasWaiting)
        {
            // Leaving the waiting state: tell the GUI the source is
            // healthy again (PRD 4.8.8).
            FireStatusCallbackIfChanged(SourceStatus::Running);
        }
        mRotationInProgress = false;
        return true;
    }

    // Branch (ii): path missing.
    if (!pathId.valid)
    {
        if (!mWaiting)
        {
            mWaiting = true;
            {
                std::lock_guard<std::mutex> lock(mMutex);
                mPartialLine.clear(); // PRD 4.8.7.i — partial cannot be completed
            }
            // Entering the waiting state: tell the GUI to swap the
            // status-bar label to "Source unavailable" (PRD 4.8.8).
            FireStatusCallbackIfChanged(SourceStatus::Waiting);
        }
        // Stay in waiting. Keep the old handle open; a future poll will
        // either see the path reappear (branch i wins via identity) or
        // stay missing.
        return false;
    }

    // Path is present and identity matches the open handle. If we were
    // previously waiting and the handle is invalid (e.g. we closed it),
    // re-open the path now.
    if (mWaiting && !IsValidHandle(mHandle))
    {
        mRotationInProgress = true;

        mHandle = OpenForTail(mPath);
        mIdentityAtOpen = FromOpenHandle(mHandle);
        mReadOffset = 0;
        mWaiting = false;

        {
            std::lock_guard<std::mutex> lock(mMutex);
            mPartialLine.clear();
        }

        mRotationCount.fetch_add(1, std::memory_order_acq_rel);
        FireRotationCallbackIfDebounced();
        FireStatusCallbackIfChanged(SourceStatus::Running);
        mRotationInProgress = false;
        return true;
    }
    if (mWaiting)
    {
        // Path reappeared while the handle was still open (rare on POSIX
        // but possible on Windows with FILE_SHARE_DELETE share mode).
        // Just clear the waiting flag and re-announce Running so the
        // GUI drops "Source unavailable".
        mWaiting = false;
        FireStatusCallbackIfChanged(SourceStatus::Running);
    }

    // Branch (iii): size shrunk on the open handle (copytruncate /
    // in-place truncate). Note we use the OPEN HANDLE size, not the path
    // size, so a rename-then-create that branch (i) failed to detect
    // (because identity collided) cannot mask this branch.
    const auto handleSizeOpt = SizeOfOpenHandle(mHandle);
    if (handleSizeOpt.has_value() && *handleSizeOpt < mReadOffset)
    {
        mRotationInProgress = true;

        // Recovery: seek to 0 on the existing handle. Do NOT re-open per
        // PRD 4.8.6.iii (the producer may rely on the share mode).
        mReadOffset = 0;

        {
            std::lock_guard<std::mutex> lock(mMutex);
            mPartialLine.clear();
        }

        mRotationCount.fetch_add(1, std::memory_order_acq_rel);
        FireRotationCallbackIfDebounced();
        mRotationInProgress = false;
        return true;
    }

    return false;
}

size_t TailingBytesProducerImpl::ReadMoreBytes()
{
    const auto sizeOpt = SizeOfOpenHandle(mHandle);
    if (!sizeOpt.has_value())
    {
        return 0;
    }
    const uint64_t size = *sizeOpt;
    if (size <= mReadOffset)
    {
        return 0;
    }
    const uint64_t available = size - mReadOffset;
    const size_t toRead = static_cast<size_t>(std::min<uint64_t>(available, mOptions.readChunkBytes));

    std::vector<char> scratch(toRead);
    const ssize_type n = ReadAtOffset(mHandle, scratch.data(), toRead, mReadOffset);
    if (n <= 0)
    {
        return 0;
    }
    mReadOffset += static_cast<uint64_t>(n);

    {
        std::lock_guard<std::mutex> lock(mMutex);
        AppendBytesAndSplitLocked(scratch.data(), static_cast<size_t>(n));
    }
    mCv.notify_all();
    return static_cast<size_t>(n);
}

void TailingBytesProducerImpl::CloseHandle() noexcept
{
    if (IsValidHandle(mHandle))
    {
        CloseNativeHandle(mHandle);
    }
    mHandle = kInvalidHandle;
}

void TailingBytesProducerImpl::AppendBytesAndSplitLocked(const char *data, size_t size)
{
    if (size == 0)
    {
        return;
    }
    mPartialLine.append(data, size);
    if (mPartialLine.empty())
    {
        return;
    }
    const size_t lastNewline = mPartialLine.find_last_of('\n');
    if (lastNewline == std::string::npos)
    {
        return;
    }
    const size_t completeBytes = lastNewline + 1;
    mReadyBuffer.insert(mReadyBuffer.end(), mPartialLine.begin(), mPartialLine.begin() + completeBytes);
    mPartialLine.erase(0, completeBytes);
}

void TailingBytesProducerImpl::FlushPartialLineLocked()
{
    if (mPartialLine.empty())
    {
        return;
    }
    mReadyBuffer.insert(mReadyBuffer.end(), mPartialLine.begin(), mPartialLine.end());
    if (mReadyBuffer.empty() || mReadyBuffer.back() != '\n')
    {
        mReadyBuffer.push_back('\n');
    }
    mPartialLine.clear();
}

void TailingBytesProducerImpl::CompactReadyBufferIfNeededLocked()
{
    // Compact when half the buffer (or 64 KiB, whichever larger) has been
    // consumed. Erasing the prefix is O(remaining); the threshold ensures
    // amortised O(1) per byte.
    constexpr size_t kMinCompactBytes = 64 * 1024;
    if (mReadyConsumed >= kMinCompactBytes && mReadyConsumed * 2 >= mReadyBuffer.size())
    {
        mReadyBuffer.erase(mReadyBuffer.begin(), mReadyBuffer.begin() + mReadyConsumed);
        mReadyConsumed = 0;
    }
}

void TailingBytesProducerImpl::FireRotationCallbackIfDebounced()
{
    const auto now = std::chrono::steady_clock::now();
    std::function<void()> cb;
    {
        std::lock_guard<std::mutex> lock(mCallbackMutex);
        if (now - mLastRotationFireTime < mOptions.rotationDebounce)
        {
            return; // within debounce window — collapse this rotation event
        }
        mLastRotationFireTime = now;
        cb = mRotationCallback;
    }
    if (cb)
    {
        cb();
    }
}

void TailingBytesProducerImpl::FireStatusCallbackIfChanged(SourceStatus status)
{
    std::function<void(SourceStatus)> cb;
    {
        std::lock_guard<std::mutex> lock(mCallbackMutex);
        if (status == mLastReportedStatus)
        {
            return; // edge-trigger — only fire on actual transitions
        }
        mLastReportedStatus = status;
        cb = mStatusCallback;
    }
    if (cb)
    {
        cb(status);
    }
}

} // namespace detail

TailingBytesProducer::TailingBytesProducer(std::filesystem::path path, size_t retentionLines, Options options)
    : mImpl(std::make_unique<detail::TailingBytesProducerImpl>(std::move(path), retentionLines, options))
{
}

TailingBytesProducer::~TailingBytesProducer() = default;

size_t TailingBytesProducer::Read(std::span<char> buffer)
{
    return mImpl->Read(buffer);
}

void TailingBytesProducer::WaitForBytes(std::chrono::milliseconds timeout)
{
    mImpl->WaitForBytes(timeout);
}

void TailingBytesProducer::Stop() noexcept
{
    mImpl->Stop();
}

bool TailingBytesProducer::IsClosed() const noexcept
{
    return mImpl->IsClosed();
}

std::string TailingBytesProducer::DisplayName() const
{
    return mImpl->DisplayName();
}

void TailingBytesProducer::SetRotationCallback(std::function<void()> callback)
{
    mImpl->SetRotationCallback(std::move(callback));
}

void TailingBytesProducer::SetStatusCallback(std::function<void(SourceStatus)> callback)
{
    mImpl->SetStatusCallback(std::move(callback));
}

size_t TailingBytesProducer::RotationCount() const noexcept
{
    return mImpl->RotationCount();
}

} // namespace loglib
