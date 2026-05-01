#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <span>
#include <string>

namespace loglib
{

class LogFile;

/// Coarse state reported by a `LogSource` via `SetStatusCallback` (PRD §6
/// *Status bar*). Surfaced to the GUI so the status-bar label can cycle
/// between `Streaming <file>` / `Paused` / `Source unavailable …` without
/// the GUI having to poll the source's internals.
enum class SourceStatus
{
    /// The source is producing bytes (or can): the open handle is
    /// valid and the tailing read is progressing as normal. This is
    /// the default state and the only state finite sources
    /// (`MappedFileSource`) ever reach.
    Running,

    /// The source is temporarily unable to produce bytes — typically
    /// because the watched file has disappeared during a
    /// delete-then-recreate rotation (PRD 4.8.8). The source keeps
    /// retrying; `Running` fires again when the file reappears and is
    /// re-opened.
    Waiting,
};

/// Abstract byte / line producer feeding a `LogParser`. Both the existing
/// memory-mapped finite-file path (`MappedFileSource`) and the future live
/// tail (`TailingFileSource`, stdin / TCP / UDP / named-pipe) implement this.
///
/// The contract is deliberately a *byte producer*, not a parser orchestrator
/// — the parser is the driver and the source the producer (PRD 4.9.2). The
/// source has no knowledge of `StreamingLogSink` or `ParserOptions`.
class LogSource
{
public:
    LogSource() = default;
    virtual ~LogSource() = default;

    LogSource(const LogSource &) = delete;
    LogSource &operator=(const LogSource &) = delete;

    LogSource(LogSource &&) = delete;
    LogSource &operator=(LogSource &&) = delete;

    /// Pull bytes since the last call into @p buffer. Returns the number of
    /// bytes written.
    ///
    /// Returns 0 on:
    ///   - **transient EOF** for live-tail sources (`IsClosed()` stays false;
    ///     the caller may park on `WaitForBytes` and try again),
    ///   - **terminal EOF** for finite sources once the file is exhausted
    ///     (`IsClosed()` flips to true and stays there),
    ///   - **after `Stop()`** has been observed; subsequent calls keep
    ///     returning 0 with `IsClosed() == true`.
    ///
    /// Empty @p buffer is well-defined and returns 0 without consuming.
    virtual size_t Read(std::span<char> buffer) = 0;

    /// Block the caller until at least one byte is available, the deadline
    /// elapses, or `Stop()` is called. Live-tail sources park on a
    /// condition variable; finite sources (`MappedFileSource`) return
    /// immediately. Spurious wakeups are allowed; callers must re-check
    /// via `Read` (PRD 4.9.2.ii).
    virtual void WaitForBytes(std::chrono::milliseconds timeout) = 0;

    /// Unblock any in-flight `Read` / `WaitForBytes`; cause subsequent
    /// calls to report terminal EOF. Safe to call from any thread,
    /// including the GUI thread during model teardown. Distinct from
    /// `ParserOptions::stopToken`: the source's `Stop` releases I/O so
    /// the parser's hot loop can observe the parser stop token at the
    /// next batch boundary (PRD 4.7.2.i, 4.9.2.iii).
    ///
    /// Idempotent: a second `Stop()` is a no-op.
    virtual void Stop() noexcept = 0;

    /// Has the source reached terminal EOF (either natural exhaustion
    /// for a finite source, or `Stop()` having been observed)?
    [[nodiscard]] virtual bool IsClosed() const noexcept = 0;

    /// Human-readable name of the source, used by the GUI status-bar
    /// (e.g. the file path's basename for a `TailingFileSource`).
    [[nodiscard]] virtual std::string DisplayName() const = 0;

    /// Optional rotation-event callback. Invoked from the source's own
    /// worker thread when the source detects a rotation (PRD 4.8.6 /
    /// 4.8.7.v); the default no-op is appropriate for finite sources
    /// such as `MappedFileSource`. Setting an empty callback clears any
    /// previously-installed one.
    virtual void SetRotationCallback(std::function<void()> callback);

    /// Optional status-change callback. Invoked from the source's own
    /// worker thread when the source transitions between
    /// `SourceStatus::Running` and `SourceStatus::Waiting` (PRD 4.8.8 /
    /// §6 *Status bar*). Edge-triggered: only called on actual
    /// transitions, not on every poll tick. Default no-op is
    /// appropriate for finite sources (`MappedFileSource`). Setting an
    /// empty callback clears any previously-installed one.
    virtual void SetStatusCallback(std::function<void(SourceStatus)> callback);

    /// Capability hook: returns true when this source is backed by a
    /// finite, memory-mapped `LogFile` (i.e. `MappedFileSource`). The
    /// `JsonParser::ParseStreaming(LogSource&, ...)` overload uses this
    /// to bypass `Read` and drive the existing TBB pipeline directly
    /// over the mmap, preserving the static-path performance bar
    /// (PRD 4.9.4, task 1.4). Default is false.
    [[nodiscard]] virtual bool IsMappedFile() const noexcept;

    /// When `IsMappedFile()` returns true, returns a non-null pointer to
    /// the underlying `LogFile`. Otherwise returns nullptr. The pointer
    /// is owned by the source; the caller must not delete it. Default
    /// returns nullptr.
    [[nodiscard]] virtual LogFile *GetMappedLogFile() noexcept;
};

} // namespace loglib
