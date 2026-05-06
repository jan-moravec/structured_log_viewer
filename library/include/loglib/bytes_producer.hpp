#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <span>
#include <string>

namespace loglib
{

/// Coarse state reported by a `BytesProducer` via `SetStatusCallback`,
/// surfaced to the GUI for the status-bar label.
enum class SourceStatus
{
    /// The producer can produce bytes; the default state finite
    /// producers never leave.
    Running,

    /// Temporarily unable to produce bytes (typically the watched file
    /// disappeared during a delete-then-recreate rotation). The
    /// producer keeps retrying and re-emits `Running` when bytes flow
    /// again.
    Waiting,
};

/// Abstract byte producer feeding the streaming pipeline via
/// `StreamLineSource`. Concrete implementations: `TailingBytesProducer`
/// (file tail), `TcpServerProducer`, `UdpServerProducer`.
///
/// Deliberately a byte producer, not a parser orchestrator: the parser
/// drives, the producer supplies bytes. Has no knowledge of
/// `LogParseSink` or `ParserOptions`. The static-file path uses
/// `FileLineSource` directly and bypasses this abstraction.
class BytesProducer
{
public:
    BytesProducer() = default;
    virtual ~BytesProducer() = default;

    BytesProducer(const BytesProducer &) = delete;
    BytesProducer &operator=(const BytesProducer &) = delete;

    BytesProducer(BytesProducer &&) = delete;
    BytesProducer &operator=(BytesProducer &&) = delete;

    /// Pull available bytes into @p buffer. Returns the number of
    /// bytes written. 0 indicates either transient EOF (live-tail; the
    /// caller can park on `WaitForBytes` and retry) or terminal EOF
    /// (`IsClosed()` flips to true). Empty @p buffer is well-defined
    /// and returns 0.
    virtual size_t Read(std::span<char> buffer) = 0;

    /// Block until at least one byte is available, the deadline
    /// elapses, or `Stop()` is called. Spurious wakeups are allowed;
    /// callers must re-check via `Read`.
    virtual void WaitForBytes(std::chrono::milliseconds timeout) = 0;

    /// Unblock any in-flight `Read`/`WaitForBytes` and cause subsequent
    /// calls to report terminal EOF. Safe from any thread (including
    /// the GUI thread during teardown). Idempotent. Distinct from
    /// `ParserOptions::stopToken` -- this releases I/O so the parser's
    /// hot loop can observe its own stop token at the next batch
    /// boundary.
    virtual void Stop() noexcept = 0;

    /// Has the producer reached terminal EOF (natural exhaustion or
    /// `Stop()` observed)?
    [[nodiscard]] virtual bool IsClosed() const noexcept = 0;

    /// Human-readable name (e.g. the file path's basename), used by
    /// the GUI status bar.
    [[nodiscard]] virtual std::string DisplayName() const = 0;

    /// Rotation-event callback, invoked from the producer's worker
    /// thread on detected rotations. Default no-op for non-rotating
    /// producers; an empty callback clears any previous installation.
    virtual void SetRotationCallback(std::function<void()> callback);

    /// Status-change callback, invoked on edge transitions between
    /// `Running` and `Waiting`. Default no-op; an empty callback
    /// clears any previous installation.
    virtual void SetStatusCallback(std::function<void(SourceStatus)> callback);
};

} // namespace loglib
