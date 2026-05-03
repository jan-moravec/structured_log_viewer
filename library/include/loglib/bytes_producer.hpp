#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <span>
#include <string>

namespace loglib
{

/// Coarse state reported by a `BytesProducer` via `SetStatusCallback`
/// (PRD §6 *Status bar*). Surfaced to the GUI so the status-bar label
/// can cycle between `Streaming <file>` / `Paused` / `Source unavailable …`
/// without the GUI having to poll the producer's internals.
enum class SourceStatus
{
    /// The producer is producing bytes (or can): the open handle is
    /// valid and the tailing read is progressing as normal. This is
    /// the default state finite producers never leave.
    Running,

    /// The producer is temporarily unable to produce bytes — typically
    /// because the watched file has disappeared during a
    /// delete-then-recreate rotation (PRD 4.8.8). The producer keeps
    /// retrying; `Running` fires again when the file reappears and is
    /// re-opened.
    Waiting,
};

/// Abstract byte producer feeding the streaming pipeline through a
/// `StreamLineSource`. The current concrete implementation is
/// `TailingBytesProducer`; future live-tail producers (stdin / TCP /
/// UDP / named-pipe) implement the same contract.
///
/// The contract is deliberately a *byte producer*, not a parser
/// orchestrator — the parser is the driver and the producer the source
/// of bytes (PRD 4.9.2). The producer has no knowledge of
/// `StreamingLogSink` or `ParserOptions`. The static-file path uses
/// `FileLineSource` directly and never goes through this abstraction.
class BytesProducer
{
public:
    BytesProducer() = default;
    virtual ~BytesProducer() = default;

    BytesProducer(const BytesProducer &) = delete;
    BytesProducer &operator=(const BytesProducer &) = delete;

    BytesProducer(BytesProducer &&) = delete;
    BytesProducer &operator=(BytesProducer &&) = delete;

    /// Pull bytes since the last call into @p buffer. Returns the number of
    /// bytes written.
    ///
    /// Returns 0 on:
    ///   - **transient EOF** for live-tail producers (`IsClosed()` stays
    ///     false; the caller may park on `WaitForBytes` and try again),
    ///   - **terminal EOF** once the producer is exhausted (`IsClosed()`
    ///     flips to true and stays there),
    ///   - **after `Stop()`** has been observed; subsequent calls keep
    ///     returning 0 with `IsClosed() == true`.
    ///
    /// Empty @p buffer is well-defined and returns 0 without consuming.
    virtual size_t Read(std::span<char> buffer) = 0;

    /// Block the caller until at least one byte is available, the deadline
    /// elapses, or `Stop()` is called. Live-tail producers park on a
    /// condition variable; finite producers return immediately. Spurious
    /// wakeups are allowed; callers must re-check via `Read`
    /// (PRD 4.9.2.ii).
    virtual void WaitForBytes(std::chrono::milliseconds timeout) = 0;

    /// Unblock any in-flight `Read` / `WaitForBytes`; cause subsequent
    /// calls to report terminal EOF. Safe to call from any thread,
    /// including the GUI thread during model teardown. Distinct from
    /// `ParserOptions::stopToken`: the producer's `Stop` releases I/O so
    /// the parser's hot loop can observe the parser stop token at the
    /// next batch boundary (PRD 4.7.2.i, 4.9.2.iii).
    ///
    /// Idempotent: a second `Stop()` is a no-op.
    virtual void Stop() noexcept = 0;

    /// Has the producer reached terminal EOF (either natural exhaustion
    /// for a finite producer, or `Stop()` having been observed)?
    [[nodiscard]] virtual bool IsClosed() const noexcept = 0;

    /// Human-readable name of the producer, used by the GUI status-bar
    /// (e.g. the file path's basename for a `TailingBytesProducer`).
    [[nodiscard]] virtual std::string DisplayName() const = 0;

    /// Optional rotation-event callback. Invoked from the producer's own
    /// worker thread when the producer detects a rotation (PRD 4.8.6 /
    /// 4.8.7.v); the default no-op is appropriate for producers that
    /// never rotate. Setting an empty callback clears any previously-
    /// installed one.
    virtual void SetRotationCallback(std::function<void()> callback);

    /// Optional status-change callback. Invoked from the producer's own
    /// worker thread when the producer transitions between
    /// `SourceStatus::Running` and `SourceStatus::Waiting` (PRD 4.8.8 /
    /// §6 *Status bar*). Edge-triggered: only called on actual
    /// transitions, not on every poll tick. Default no-op is
    /// appropriate for producers that never become unavailable. Setting
    /// an empty callback clears any previously-installed one.
    virtual void SetStatusCallback(std::function<void(SourceStatus)> callback);
};

} // namespace loglib
