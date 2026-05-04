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

/// `BytesProducer` over a file being actively written. Pre-fills the
/// last N complete lines from disk and then tails the producer's
/// appends, surviving rename-and-create / copy-truncate / in-place
/// truncate / delete-then-recreate rotations.
///
/// Design summary:
///   - Buffered POSIX `read(2)` / Windows `ReadFile`. No mmap on the
///     tail because rotations would invalidate the mapping or block
///     the producer.
///   - One worker thread per producer. It polls the file size at a
///     configurable cadence (default 250 ms) and is also signalled by
///     `efsw` filesystem events so the typical wake latency is the
///     watcher's, not the poll's.
///   - Rotation detection evaluates branches in order: identity /
///     missing / size-shrunk. Bursts within 1 s coalesce into a single
///     rotation event.
///   - `Read()` drains a byte queue holding only complete lines; the
///     partial-line buffer is separate, discarded on rotation, flushed
///     as a synthetic last line on `Stop`.
class TailingBytesProducer final : public BytesProducer
{
public:
    /// Tuning knobs, exposed mainly so tests on slow CI runners can
    /// shrink poll intervals and disable the native watcher.
    struct Options
    {
        /// Worker poll cadence when no filesystem event has fired.
        std::chrono::milliseconds pollInterval = std::chrono::milliseconds(250);

        /// Rotations within this window coalesce into a single event.
        std::chrono::milliseconds rotationDebounce = std::chrono::milliseconds(1000);

        /// Bytes per tail-read syscall. 64 KiB amortises syscall cost
        /// while bounding the partial-line buffer.
        size_t readChunkBytes = 64 * 1024;

        /// Bytes per pre-fill backwards chunk. Separate from
        /// `readChunkBytes` so tests can shrink it independently.
        size_t prefillChunkBytes = 64 * 1024;

        /// Upper bound on the backwards pre-fill scan. Protects against
        /// files with no newlines or absurdly large `retentionLines`
        /// on multi-GB files. On overflow, pre-fill yields zero lines
        /// and tailing begins from EOF.
        size_t prefillMaxScanBytes = 16 * 1024 * 1024;

        /// Skip the `efsw` watcher and rely on polling alone. Used by
        /// tests on CI runners where filesystem events are unreliable.
        bool disableNativeWatcher = false;
    };

    /// Construct and spawn the worker. Pre-fill drains the last
    /// @p retentionLines complete lines into the byte queue; tailing
    /// begins from the byte offset pre-fill stopped at.
    ///
    /// Throws if the initial open fails; transient missing-path errors
    /// during tailing are handled internally (rotation branch ii) and
    /// never propagate.
    TailingBytesProducer(std::filesystem::path path, size_t retentionLines, Options options = Options{});

    ~TailingBytesProducer() override;

    TailingBytesProducer(const TailingBytesProducer &) = delete;
    TailingBytesProducer &operator=(const TailingBytesProducer &) = delete;
    TailingBytesProducer(TailingBytesProducer &&) = delete;
    TailingBytesProducer &operator=(TailingBytesProducer &&) = delete;

    size_t Read(std::span<char> buffer) override;

    void WaitForBytes(std::chrono::milliseconds timeout) override;

    /// Unblock any in-flight `Read`/`WaitForBytes`, flush the partial
    /// line as a synthetic last line (unless a rotation is in
    /// progress), and signal the worker to exit. Idempotent. Safe
    /// from any thread.
    void Stop() noexcept override;

    [[nodiscard]] bool IsClosed() const noexcept override;

    [[nodiscard]] std::string DisplayName() const override;

    void SetRotationCallback(std::function<void()> callback) override;

    void SetStatusCallback(std::function<void(SourceStatus)> callback) override;

    /// Total rotations the worker has observed since construction.
    /// Used by tests to assert debounce coalescing (the user-visible
    /// callback fires less often than this counter increments).
    [[nodiscard]] size_t RotationCount() const noexcept;

private:
    std::unique_ptr<internal::TailingBytesProducerImpl> mImpl;
};

} // namespace loglib
