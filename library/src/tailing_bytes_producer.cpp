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
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace
{
// `ssize_t` is POSIX-only; alias to a portable signed type so the I/O
// helpers below have a single signature on all platforms.
using SsizeType = std::int64_t;
} // namespace

namespace loglib
{

namespace
{

#ifdef _WIN32
// `INVALID_HANDLE_VALUE` is a reinterpret_cast and not a constant
// expression in MSVC's C++23 mode (C2131); use a plain `const` global.
const internal::NativeFileHandle INVALID_HANDLE = INVALID_HANDLE_VALUE;
#else
constexpr internal::NativeFileHandle INVALID_HANDLE = -1;
#endif

bool IsValidHandle(internal::NativeFileHandle handle) noexcept
{
#ifdef _WIN32
    return handle != INVALID_HANDLE_VALUE && handle != nullptr;
#else
    return handle >= 0;
#endif
}

void CloseNativeHandle(internal::NativeFileHandle handle) noexcept
{
    if (!IsValidHandle(handle))
    {
        return;
    }
#ifdef _WIN32
    ::CloseHandle(handle);
#else
    ::close(handle);
#endif
}

/// Canonicalize @p path so the efsw watcher is always installed on an
/// absolute parent directory. Covers relative paths (`"app.log"`), `..`
/// components, and Windows root-relative paths like `"C:foo.log"`
/// whose `parent_path()` would otherwise be empty. Falls back to the
/// untouched input on error.
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

/// Open @p path for read with maximum producer-friendly share flags.
/// Returns `INVALID_HANDLE` on failure.
internal::NativeFileHandle OpenForTail(const std::filesystem::path &path) noexcept
{
#ifdef _WIN32
    // FILE_SHARE_DELETE is critical: without it the producer cannot
    // rename or unlink the file. FILE_FLAG_SEQUENTIAL_SCAN hints the
    // cache manager to read ahead.
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

/// Sample the open handle's current size. Returns `std::nullopt` if the
/// OS call fails (e.g. the handle has been closed or the inode is gone).
std::optional<uint64_t> SizeOfOpenHandle(internal::NativeFileHandle handle) noexcept
{
    if (!IsValidHandle(handle))
    {
        return std::nullopt;
    }
#ifdef _WIN32
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

/// Pread-style read: up to @p size bytes from @p offset, without
/// advancing a persistent file pointer. Short reads at EOF are normal.
/// Returns -1 on hard error.
SsizeType ReadAtOffset(internal::NativeFileHandle handle, void *out, size_t size, uint64_t offset) noexcept
{
    if (!IsValidHandle(handle) || size == 0)
    {
        return 0;
    }
#ifdef _WIN32
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
    return static_cast<SsizeType>(bytesRead);
#else
    SsizeType total = 0;
    char *outChars = static_cast<char *>(out);
    while (total < static_cast<SsizeType>(size))
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
        total += static_cast<SsizeType>(n);
    }
    return total;
#endif
}

} // namespace

namespace internal
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
    /// Worker-thread-only. Returns true if a rotation branch matched.
    bool DetectAndRecoverRotation();
    /// Worker-thread-only. Reads up to `options.readChunkBytes`, splits
    /// at `\n`, and pushes complete lines into `mReadyBuffer`.
    size_t ReadMoreBytes();
    /// Worker-thread-only.
    void CloseHandle() noexcept;
    /// Caller must hold `mMutex`.
    void AppendBytesAndSplitLocked(const char *data, size_t size);
    /// Caller must hold `mMutex`. Appends `mPartialLine` to ready (with
    /// a synthetic `\n` if absent) and clears it.
    void FlushPartialLineLocked();
    /// Caller must hold `mMutex`. Compacts after substantial consumption.
    void CompactReadyBufferIfNeededLocked();
    /// Fires the rotation callback debounced by `rotationDebounce`.
    void FireRotationCallbackIfDebounced();

    /// Edge-triggered: fires only on transition. Caller must NOT hold
    /// `mMutex`.
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

    // Separate mutex so we never call user callbacks while holding the
    // busy `mMutex`.
    mutable std::mutex mCallbackMutex;
    std::function<void()> mRotationCallback;
    std::function<void(SourceStatus)> mStatusCallback;
    std::chrono::steady_clock::time_point mLastRotationFireTime;
    SourceStatus mLastReportedStatus = SourceStatus::Running;

    // Worker-thread-only state (no lock needed):
    NativeFileHandle mHandle = INVALID_HANDLE;
    FileIdentity mIdentityAtOpen;
    uint64_t mReadOffset = 0;
    bool mRotationInProgress = false;

    // efsw integration. Destroyed from the dtor *after* worker join so
    // the listener can never fire on a half-destroyed impl.
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
    // Canonicalize up front so every downstream computation (basename,
    // display name, watcher parent) sees an absolute path.
    : mPath(CanonicalizeWatchedPath(std::move(path))),
      mFileName(mPath.filename().string()),
      mDisplayName(mPath.string()),
      mRetentionLines(std::max<size_t>(retentionLines, 1)),
      mOptions(options),
      mHandle(OpenForTail(mPath))
{
    // Open synchronously. Transient ENOENT during tailing is handled by
    // the worker's rotation branch (ii), not here.

    if (!IsValidHandle(mHandle))
    {
        const std::error_code ec(errno, std::generic_category());
        throw std::system_error(ec, "TailingBytesProducer: failed to open '" + mDisplayName + "'");
    }
    mIdentityAtOpen = FromOpenHandle(mHandle);

    // Pre-fill runs from the worker's prologue (not here) so a multi-GB
    // backwards-newline scan never blocks the GUI thread. `Read` returns
    // 0 until pre-fill lands bytes; the parser parks on `WaitForBytes`.

    // Watch the parent directory: Linux inotify / macOS FSEvents work at
    // directory granularity; the listener filters by basename. The
    // `current_path()` fallback below covers pathological inputs where
    // `weakly_canonical` declined (symlink loop, OOM, FS rejection).
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
            // Negative WatchID = efsw failure; fall through to polling.
        }
        catch (...)
        {
            // efsw can throw on resource exhaustion; polling is the
            // graceful fallback.
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
    // Tear down watcher AFTER worker join so the listener never fires
    // on a partially-destroyed impl.
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

    const std::scoped_lock lock(mMutex);
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
        return; // non-blocking
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
        const std::scoped_lock lock(mMutex);
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
    const std::scoped_lock lock(mMutex);
    return (mReadyBuffer.size() - mReadyConsumed) == 0;
}

std::string TailingBytesProducerImpl::DisplayName() const
{
    return mDisplayName;
}

void TailingBytesProducerImpl::SetRotationCallback(std::function<void()> callback)
{
    const std::scoped_lock lock(mCallbackMutex);
    mRotationCallback = std::move(callback);
}

void TailingBytesProducerImpl::SetStatusCallback(std::function<void(SourceStatus)> callback)
{
    // Mirror `TcpServerProducer` / `UdpServerProducer`: snapshot the
    // last-reported status under the lock and replay it once outside
    // so a GUI consumer that subscribes after a `Waiting` transition
    // (e.g. file went missing before the model wired the callback)
    // immediately sees the live state instead of an inconsistent
    // implicit `Running` baseline. Edge-triggered firing inside
    // `FireStatusCallbackIfChanged` is unaffected because we do not
    // touch `mLastReportedStatus` here.
    std::function<void(SourceStatus)> snapshot;
    SourceStatus current{};
    {
        const std::scoped_lock lock(mCallbackMutex);
        mStatusCallback = std::move(callback);
        snapshot = mStatusCallback;
        current = mLastReportedStatus;
    }
    if (snapshot)
    {
        snapshot(current);
    }
}

size_t TailingBytesProducerImpl::RotationCount() const noexcept
{
    return mRotationCount.load(std::memory_order_acquire);
}

void TailingBytesProducerImpl::OnWatcherEvent() noexcept
{
    {
        const std::scoped_lock lock(mMutex);
        mWatcherEventPending = true;
    }
    mCv.notify_all();
}

void TailingBytesProducerImpl::WorkerMain()
{
    // Pre-fill runs from the worker prologue (not the ctor) so a
    // multi-GB backwards-newline scan never blocks the caller.
    if (!mStopRequested.load(std::memory_order_acquire))
    {
        Prefill();
    }

    while (!mStopRequested.load(std::memory_order_acquire))
    {
        bool didWork = false;

        if (DetectAndRecoverRotation())
        {
            didWork = true;
        }

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
            // Park until a watcher event fires, Stop is requested, or
            // the poll interval elapses.
            std::unique_lock<std::mutex> lock(mMutex);
            mCv.wait_for(lock, mOptions.pollInterval, [&] {
                return mWatcherEventPending || mStopRequested.load(std::memory_order_acquire);
            });
            mWatcherEventPending = false;
        }
        else
        {
            const std::scoped_lock lock(mMutex);
            mWatcherEventPending = false;
        }
    }

    // Flush the partial-line buffer as a synthetic final line, unless
    // we're mid-rotation.
    {
        const std::scoped_lock lock(mMutex);
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

    // Walk backwards in `prefillChunkBytes` slices, counting newlines
    // until we have N+1 (the byte after the (N+1)-th newline from the
    // end is the start of the last N complete lines).
    const size_t targetNewlines = mRetentionLines + 1;
    uint64_t pos = fileSize;
    size_t newlinesSeen = 0;
    uint64_t lineStart = 0; // default: keep the whole file
    bool boundaryFound = false;
    std::vector<char> chunk;
    chunk.resize(mOptions.prefillChunkBytes);

    // Bound the pre-fill scan so a newline-less or pathologically large
    // file can't wedge the worker. On overflow, skip to EOF.
    uint64_t scannedBytes = 0;

    while (pos > 0 && !boundaryFound)
    {
        const uint64_t chunkSize = std::min<uint64_t>(pos, mOptions.prefillChunkBytes);
        pos -= chunkSize;
        const SsizeType n = ReadAtOffset(mHandle, chunk.data(), static_cast<size_t>(chunkSize), pos);
        if (n <= 0)
        {
            // Read failed; abandon prefill and tail from file_size.
            mReadOffset = fileSize;
            return;
        }
        scannedBytes += static_cast<uint64_t>(n);
        for (auto i = static_cast<size_t>(n); i > 0; --i)
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
            // Scan budget exhausted; yield no lines and tail from EOF.
            mReadOffset = fileSize;
            return;
        }
    }

    if (lineStart < fileSize)
    {
        const uint64_t toRead = fileSize - lineStart;
        std::vector<char> body(static_cast<size_t>(toRead));
        const SsizeType n = ReadAtOffset(mHandle, body.data(), body.size(), lineStart);
        if (n > 0)
        {
            const std::scoped_lock lock(mMutex);
            AppendBytesAndSplitLocked(body.data(), static_cast<size_t>(n));
        }
    }

    // Tailing resumes from EOF as observed at pre-fill time. Any partial
    // last line was retained by AppendBytesAndSplitLocked and seeds the
    // tail's partial-line state.
    mReadOffset = fileSize;

    // Wake any consumer parked on `WaitForBytes` so they don't sleep
    // until `pollInterval` after pre-fill data lands.
    mCv.notify_all();
}

bool TailingBytesProducerImpl::DetectAndRecoverRotation()
{
    const FileIdentity pathId = FromPath(mPath);
    const FileIdentity handleId = FromOpenHandle(mHandle);

    // Branch (i): identity changed (rename-and-create or
    // delete-then-recreate finished between polls).
    if (pathId.valid && handleId.valid && pathId != handleId)
    {
        mRotationInProgress = true;

        CloseHandle();
        mHandle = OpenForTail(mPath);
        mIdentityAtOpen = FromOpenHandle(mHandle);
        mReadOffset = 0;
        const bool wasWaiting = mWaiting;
        mWaiting = false;

        {
            const std::scoped_lock lock(mMutex);
            mPartialLine.clear();
        }

        mRotationCount.fetch_add(1, std::memory_order_acq_rel);
        FireRotationCallbackIfDebounced();
        if (wasWaiting)
        {
            FireStatusCallbackIfChanged(SourceStatus::Running);
        }
        mRotationInProgress = false;
        return true;
    }

    // Branch (ii): path missing. Keep the old handle open; future
    // polls will either see the path reappear (branch i) or stay
    // missing.
    if (!pathId.valid)
    {
        if (!mWaiting)
        {
            mWaiting = true;
            {
                const std::scoped_lock lock(mMutex);
                mPartialLine.clear();
            }
            FireStatusCallbackIfChanged(SourceStatus::Waiting);
        }
        return false;
    }

    // Path present and identity matches. If we were waiting and the
    // handle is invalid, re-open.
    if (mWaiting && !IsValidHandle(mHandle))
    {
        mRotationInProgress = true;

        mHandle = OpenForTail(mPath);
        mIdentityAtOpen = FromOpenHandle(mHandle);
        mReadOffset = 0;
        mWaiting = false;

        {
            const std::scoped_lock lock(mMutex);
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
        // Path reappeared while our handle stayed open (rare on POSIX,
        // possible on Windows under FILE_SHARE_DELETE).
        mWaiting = false;
        FireStatusCallbackIfChanged(SourceStatus::Running);
    }

    // Branch (iii): copytruncate / in-place truncate. Use the OPEN
    // HANDLE size (not path size) so an identity-colliding rotation
    // can't mask this branch.
    const auto handleSizeOpt = SizeOfOpenHandle(mHandle);
    if (handleSizeOpt.has_value() && *handleSizeOpt < mReadOffset)
    {
        mRotationInProgress = true;

        // Seek to 0 on the existing handle (don't re-open: the producer
        // may rely on our share mode).
        mReadOffset = 0;

        {
            const std::scoped_lock lock(mMutex);
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
    const auto toRead = static_cast<size_t>(std::min<uint64_t>(available, mOptions.readChunkBytes));

    std::vector<char> scratch(toRead);
    const SsizeType n = ReadAtOffset(mHandle, scratch.data(), toRead, mReadOffset);
    if (n <= 0)
    {
        return 0;
    }
    mReadOffset += static_cast<uint64_t>(n);

    {
        const std::scoped_lock lock(mMutex);
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
    mHandle = INVALID_HANDLE;
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
    // Compact once half the buffer (or 64 KiB, whichever is larger) is
    // consumed: amortised O(1) per byte.
    constexpr size_t MIN_COMPACT_BYTES = 64 * 1024;
    if (mReadyConsumed >= MIN_COMPACT_BYTES && mReadyConsumed * 2 >= mReadyBuffer.size())
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
        const std::scoped_lock lock(mCallbackMutex);
        if (now - mLastRotationFireTime < mOptions.rotationDebounce)
        {
            return; // collapse rapid back-to-back rotations
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
        const std::scoped_lock lock(mCallbackMutex);
        if (status == mLastReportedStatus)
        {
            return; // edge-triggered: only fire on transitions
        }
        mLastReportedStatus = status;
        cb = mStatusCallback;
    }
    if (cb)
    {
        cb(status);
    }
}

} // namespace internal

TailingBytesProducer::TailingBytesProducer(std::filesystem::path path, size_t retentionLines)
    : TailingBytesProducer(std::move(path), retentionLines, Options{})
{
}

TailingBytesProducer::TailingBytesProducer(std::filesystem::path path, size_t retentionLines, Options options)
    : mImpl(std::make_unique<internal::TailingBytesProducerImpl>(std::move(path), retentionLines, options))
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
