#pragma once

#include <loglib/stop_token.hpp>
#include <loglib/streaming_log_sink.hpp>

#include <QObject>
#include <QPointer>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

class LogModel;

/// Qt adapter that forwards `loglib::StreamingLogSink` callbacks from a TBB
/// worker thread to a `LogModel` on the GUI thread. Generation/stop-source
/// owned by the GUI thread; worker callbacks post via QueuedConnection and
/// drop on generation mismatch. The parse thread itself is owned by the
/// caller (typically `MainWindow` via `QtConcurrent::run`).
///
/// Live-tail Pause/Resume (PRD 4.2.2.v / 4.10.1):
///   While `Pause()` is in effect, the worker-side `OnBatch` redirects
///   parsed batches into an internal `mPausedBatches` vector instead of
///   posting per-batch `QueuedConnection` lambdas. `Resume()` coalesces
///   the buffer into a single `StreamedBatch` and posts it once. The
///   paused buffer is bounded (4.2.2.iv) by the configured retention cap
///   so an indefinitely-paused viewer attached to a noisy producer cannot
///   OOM.
class QtStreamingLogSink : public QObject, public loglib::StreamingLogSink
{
    Q_OBJECT

public:
    explicit QtStreamingLogSink(LogModel *model, QObject *parent = nullptr);
    ~QtStreamingLogSink() override = default;

    /// Opens a fresh parse generation and returns the stop_token to install
    /// on `ParserOptions::stopToken`. Only arms the sink; the worker is
    /// spawned elsewhere. Also resets the Pause flag and drops any leftover
    /// paused-buffer content from a prior session. GUI thread.
    loglib::StopToken Arm();

    /// Non-blocking cooperative cancel. Does **not** bump the generation —
    /// the worker's drain-phase `OnBatch`/`OnFinished` still need their
    /// captured generation to match. Pair with `DropPendingBatches()` after
    /// joining the worker. GUI thread.
    void RequestStop();

    /// Bumps the sink generation so already-queued GUI-thread lambdas
    /// short-circuit. Must be called **after** `waitForFinished()` returns
    /// so the worker cannot capture the new generation. Also clears the
    /// paused buffer. GUI thread.
    void DropPendingBatches();

    /// Returns true while a stream is armed but `EndStreaming` /
    /// `DropPendingBatches` has not run yet. GUI thread; used by
    /// `MainWindow` to gate the streaming toolbar visibility (task 4.12).
    [[nodiscard]] bool IsActive() const noexcept;

    /// Engage the worker-side pause flag. New batches go into the paused
    /// buffer rather than the GUI thread's queue. Idempotent. GUI thread.
    void Pause() noexcept;

    /// Drain the paused buffer into the GUI thread as a single coalesced
    /// batch and turn the pause flag off. Idempotent; resuming when not
    /// paused is a no-op (the buffer is empty). GUI thread.
    void Resume();

    /// Whether `Pause()` is currently in effect. GUI thread.
    [[nodiscard]] bool IsPaused() const noexcept;

    /// Number of buffered rows (both static-path `LogLine`s and live-tail
    /// `StreamLogLine`s) currently held in the paused buffer. Reflected in
    /// the status-bar `K buffered` indicator. GUI thread.
    [[nodiscard]] size_t PausedLineCount() const;

    /// Configure the retention cap used to bound the paused buffer
    /// (PRD 4.2.2.iv). `LogModel::SetRetentionCap` calls this so the
    /// worker observes the limit without crossing the thread boundary.
    /// `cap == 0` disables the bound. GUI thread (worker reads atomically).
    void SetRetentionCap(size_t cap) noexcept;

    /// Trim the paused buffer to at most @p maxBufferedLines rows by
    /// dropping the oldest entries (PRD 4.5.5.ii — "trims the paused buffer
    /// per 4.3 so the invariant holds"). GUI thread.
    void TrimPausedBufferTo(size_t maxBufferedLines);

    /// Drains the paused buffer into a single coalesced batch and returns
    /// it. Used by `LogModel`'s teardown path to flush already-parsed
    /// rows into the visible model before tear-down (PRD 4.7.3). Empty
    /// optional means the buffer was already empty.
    std::optional<loglib::StreamedBatch> TakePausedBuffer();

    /// The canonical KeyIndex (the model's LogTable's). Thread-safe.
    loglib::KeyIndex &Keys() override;

    void OnStarted() override;
    void OnBatch(loglib::StreamedBatch batch) override;
    void OnFinished(bool cancelled) override;

private:
    /// Coalesces one or more `StreamedBatch`es into a single move-target
    /// batch. The first batch keeps its `firstLineNumber`; every other
    /// field (`lines`, `streamLines`, `localLineOffsets`, `errors`,
    /// `newKeys`) is concatenated. Used by `Resume()` and
    /// `TakePausedBuffer()`. **All five vectors must be carried**:
    /// pausing a static-streaming session (reachable from the **Stream**
    /// menu) buffers `lines`/`localLineOffsets`-bearing batches whose
    /// rows would otherwise be silently lost on Resume.
    static loglib::StreamedBatch CoalesceLocked(std::vector<loglib::StreamedBatch> &&batches);

    /// Total buffered row count (`lines.size() + streamLines.size()`)
    /// across the paused buffer. Caller must hold `mPausedMutex`.
    size_t PausedLineCountLocked() const;

    QPointer<LogModel> mModel;
    std::atomic<uint64_t> mGeneration{0};
    std::optional<loglib::StopSource> mStopSource;

    /// Worker reads atomically inside `OnBatch`; GUI writes via
    /// `Pause()` / `Resume()`.
    std::atomic<bool> mPaused{false};

    /// Retention cap for the paused buffer. `0` means unbounded
    /// (the visible model's own eviction will catch up on Resume).
    /// Worker reads atomically; GUI writes via `SetRetentionCap`.
    std::atomic<size_t> mRetentionCap{0};

    /// Number of times the paused buffer dropped its oldest entries
    /// because adding the latest batch would have breached
    /// `mRetentionCap`. Exposed for tests / future status-bar telemetry.
    std::atomic<uint64_t> mPausedDropCount{0};

    /// Paused-buffer mutex. The worker thread appends to / trims from
    /// `mPausedBatches` while the GUI thread reads its size for the
    /// status bar and drains it on Resume.
    mutable std::mutex mPausedMutex;
    std::vector<loglib::StreamedBatch> mPausedBatches;

    /// `IsActive()` source of truth: arms with `Arm()`, clears with
    /// `OnFinished` (matching generation) or `DropPendingBatches()`.
    /// GUI thread.
    bool mActive = false;
};
