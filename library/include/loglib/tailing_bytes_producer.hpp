#pragma once

#include "bytes_producer.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>

namespace loglib
{

namespace internal
{
class TailingBytesProducerImpl; // pimpl forward decl
}

/// `BytesProducer` over a file that is being **actively written to**.
/// Produces the last `N` complete lines on disk first (pre-fill) and then
/// appends new lines as the producer writes them,
/// surviving the rotation patterns enumerated in –4.
///
/// **Design summary**:
///   - Buffered POSIX `read(2)` / Windows `ReadFile` via `std::ifstream`.
///     **No mmap** on the tail — rotations
///     would invalidate the mapping or block the producer.
///   - One dedicated worker thread per producer, spawned at construction.
///     The worker polls the file size at a configurable cadence (default
///     250 ms) and is also signalled by `efsw` filesystem events so that
///     the typical wake latency is the watcher's, not the polling
///     fallback's.
///   - Rotation detection runs in branch order (i identity / ii missing /
///     iii size shrunk).8.6; rapid bursts (<1 s) collapse into
///     a single rotation event.
///   - `Read(span)` drains a per-producer byte queue (only complete lines
///     ever land in it; the partial-line buffer lives separately and is
///     discarded on rotation / flushed on `Stop`
///     *Line buffering*).
///   - `Stop()` is distinct from `ParserOptions::stopToken`:
///     `Stop` releases I/O so the parser hot loop can observe the token
///     at the next batch boundary.
class TailingBytesProducer final : public BytesProducer
{
public:
    /// Tuning knobs exposed to tests so that the polling fallback / rotation
    /// debounce / I/O chunk size can be made deterministic on slow CI
    /// runners.
    struct Options
    {
        /// How often the worker re-evaluates the rotation branches when no
        /// native filesystem event has fired. Default 250 ms.
        std::chrono::milliseconds pollInterval = std::chrono::milliseconds(250);

        /// Multiple rotations within this window collapse into a single
        /// rotation event.
        std::chrono::milliseconds rotationDebounce = std::chrono::milliseconds(1000);

        /// Bytes per `read()` syscall. 64 KiB matches the pre-fill chunk
        /// and is large enough to amortise syscall cost while
        /// keeping the partial-line buffer's worst case bounded.
        size_t readChunkBytes = 64 * 1024;

        /// Bytes per pre-fill backwards chunk. Independent of
        /// `readChunkBytes` so tests can shrink it without affecting the
        /// tail-read syscall size.
        size_t prefillChunkBytes = 64 * 1024;

        /// Upper bound on how many bytes pre-fill will scan backwards
        /// while looking for the last `retentionLines` newlines before
        /// giving up and seeking to EOF. The default of
        /// 16 MiB protects against files with no newlines and against
        /// an unrealistically large `retentionLines` on a multi-GB
        /// file. On overflow, pre-fill yields zero lines and tailing
        /// begins from EOF — new lines appended by the producer from
        /// that point on are still captured.
        size_t prefillMaxScanBytes = 16 * 1024 * 1024;

        /// Skip spawning the `efsw` watcher and rely on the polling loop
        /// only. Used by tests on CI runners where filesystem events are
        /// unreliable; production code uses the default (false).
        bool disableNativeWatcher = false;
    };

    /// Construct and immediately spawn the worker. Pre-fill drains the
    /// last @p retentionLines complete lines from disk into the byte
    /// queue; tailing then begins from the byte offset where pre-fill
    /// stopped consuming.
    ///
    /// Throws `std::system_error` (or a thin wrapper) if the file cannot
    /// be opened on the initial attempt; transient missing-path errors
    /// during tailing are recovered by branch (ii) of the rotation
    /// detector and do **not** propagate as exceptions.
    TailingBytesProducer(std::filesystem::path path, size_t retentionLines, Options options = Options{});

    ~TailingBytesProducer() override;

    TailingBytesProducer(const TailingBytesProducer &) = delete;
    TailingBytesProducer &operator=(const TailingBytesProducer &) = delete;
    TailingBytesProducer(TailingBytesProducer &&) = delete;
    TailingBytesProducer &operator=(TailingBytesProducer &&) = delete;

    /// Drain up to `buffer.size()` bytes from the byte queue. Returns 0 on
    /// transient EOF (`IsClosed() == false`) — caller is expected to park
    /// on `WaitForBytes` and retry. Returns 0 with `IsClosed() == true`
    /// once `Stop()` has been observed *and* the queue has been fully
    /// drained.
    size_t Read(std::span<char> buffer) override;

    /// Park until at least one byte is available, the timeout elapses, or
    /// `Stop()` is called. Spurious wakeups allowed — callers re-check via
    /// `Read`.
    void WaitForBytes(std::chrono::milliseconds timeout) override;

    /// Unblock any in-flight `Read` / `WaitForBytes`, flush the partial
    /// line as a synthetic last line if no rotation is concurrently in
    /// progress, and signal the
    /// worker to exit. Idempotent. Safe from any thread, including the
    /// GUI thread during model teardown.
    void Stop() noexcept override;

    [[nodiscard]] bool IsClosed() const noexcept override;

    [[nodiscard]] std::string DisplayName() const override;

    void SetRotationCallback(std::function<void()> callback) override;

    void SetStatusCallback(std::function<void(SourceStatus)> callback) override;

    /// Returns the number of rotations detected since construction. Used
    /// by tests (the rotation callback alone is not enough for a test to
    /// reliably observe debounce coalescing).
    [[nodiscard]] size_t RotationCount() const noexcept;

private:
    std::unique_ptr<internal::TailingBytesProducerImpl> mImpl;
};

} // namespace loglib
