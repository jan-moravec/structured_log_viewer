#pragma once

#include "bounded_batch_queue.hpp"

#include <loglib/log_parse_sink.hpp>
#include <loglib/stop_token.hpp>

#include <QObject>
#include <QPointer>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

class LogModel;

/// Qt adapter that forwards `loglib::LogParseSink` callbacks from a worker
/// thread to a `LogModel` on the GUI thread. Generation/stop-source live on
/// the GUI thread; worker callbacks enqueue into a bounded SPSC queue and
/// schedule a single GUI-thread drain lambda per drain epoch.
///
/// Back-pressure: `OnBatch` blocks the worker once `mPending` holds
/// `kPendingCapacity` items (default 32). The block propagates back
/// through `BatchCoalescer` -> TBB Stage C -> Stage A, so the parser
/// runs at the GUI's pace instead of producing unboundedly. `RequestStop`
/// pairs `mStopSource->request_stop()` with `mPending.NotifyStop()` so a
/// blocked worker wakes immediately.
///
/// Live-tail Pause/Resume: while paused the worker buffers parsed batches
/// internally instead of enqueuing them; `Resume()` coalesces the buffer
/// and posts it once. The paused buffer is bounded by the retention cap
/// so an indefinitely-paused noisy producer cannot OOM. Pause path and
/// bounded-queue path are disjoint.
class QtStreamingLogSink : public QObject, public loglib::LogParseSink
{
    Q_OBJECT

public:
    /// Resident-batch ceiling between worker and GUI under steady state
    /// streaming. With current `STATIC_BATCH_FLUSH_LINES = 1000` this
    /// caps in-flight rows at ~32k regardless of file size.
    static constexpr std::size_t PENDING_CAPACITY_DEFAULT = 32;

    /// `pendingCapacity` is exposed for tests that want to exercise the
    /// blocking path with small caps; production callers should rely on
    /// the default.
    explicit QtStreamingLogSink(
        LogModel *model, QObject *parent = nullptr, std::size_t pendingCapacity = PENDING_CAPACITY_DEFAULT
    );
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

    /// True while a stream is armed but neither `EndStreaming` nor
    /// `DropPendingBatches` has run. GUI thread.
    [[nodiscard]] bool IsActive() const noexcept;

    /// Engage the worker-side pause flag. New batches go into the paused
    /// buffer instead of the GUI thread's queue. Idempotent. GUI thread.
    void Pause() noexcept;

    /// Drain the paused buffer to the GUI thread as a single coalesced
    /// batch and clear the pause flag. Idempotent. GUI thread.
    void Resume();

    /// Whether `Pause()` is currently in effect. GUI thread.
    [[nodiscard]] bool IsPaused() const noexcept;

    /// Number of rows currently held in the paused buffer. GUI thread.
    [[nodiscard]] size_t PausedLineCount() const;

    /// Lines dropped from the paused buffer since `Arm()` because the
    /// retention cap was breached. Surfaced in the status bar so a long
    /// pause on a noisy producer does not silently lose rows.
    [[nodiscard]] uint64_t PausedDropCount() const noexcept;

    /// Set the retention cap that bounds the paused buffer. `cap == 0`
    /// disables the bound.
    void SetRetentionCap(size_t cap) noexcept;

    /// Drop oldest entries until at most @p maxBufferedLines rows remain.
    /// GUI thread.
    void TrimPausedBufferTo(size_t maxBufferedLines);

    /// Drains the paused buffer into a single coalesced batch. Used by
    /// `LogModel`'s teardown to flush already-parsed rows into the
    /// visible model before tear-down. Empty optional iff the buffer was
    /// already empty.
    std::optional<loglib::StreamedBatch> TakePausedBuffer();

    /// GUI-thread fallback used by `LogModel`'s teardown. Drains the
    /// bounded queue and applies any rows under the still-valid
    /// generation. Normally the GUI gets here via the lazy-scheduled
    /// `Drain` lambda; this method is a defensive backstop for the
    /// teardown path when a worker enqueued items but never reached
    /// the lambda post (e.g. exited mid-`OnBatch`).
    void DrainNow();

    /// Bounded-queue capacity for diagnostics / tests.
    [[nodiscard]] std::size_t PendingCapacity() const noexcept;

    /// Number of `OnBatch` invocations whose batches were dropped
    /// because the bounded queue was full when `RequestStop` woke the
    /// blocked worker. Resets on `Arm()`. Surfaced by `LogModel` as
    /// part of the streaming-error report so users notice that some
    /// rows were lost between the parser and the GUI.
    [[nodiscard]] std::size_t BatchesDroppedDuringShutdown() const noexcept;

    /// The canonical KeyIndex (the model's LogTable's). Thread-safe.
    loglib::KeyIndex &Keys() override;

    void OnStarted() override;
    void OnBatch(loglib::StreamedBatch batch) override;
    void OnFinished(bool cancelled) override;

private:
    /// Concatenates the batches into one. The first batch's
    /// `firstLineNumber` is preserved; all other vectors are appended.
    /// Used by `Resume()`, `TakePausedBuffer()`, and `DrainGeneration`.
    static loglib::StreamedBatch CoalesceLocked(std::vector<loglib::StreamedBatch> batches);

    /// Total row count across `mPausedBatches`. Caller must hold
    /// `mPausedMutex`.
    size_t PausedLineCountLocked() const;

    /// GUI-thread. Pulls everything currently in `mPending` and applies
    /// it to the model iff `gen` still matches the live generation.
    /// Posted lazily by `OnBatch` whenever the queue transitions from
    /// "no drain scheduled" to "drain scheduled".
    void DrainGeneration(uint64_t gen);

    QPointer<LogModel> mModel;
    std::atomic<uint64_t> mGeneration{0};
    std::optional<loglib::StopSource> mStopSource;

    /// Worker reads in `OnBatch`; GUI writes via `Pause()` / `Resume()`.
    std::atomic<bool> mPaused{false};

    /// Paused-buffer cap (`0` means unbounded). Worker reads atomically.
    std::atomic<size_t> mRetentionCap{0};

    /// Lines dropped from the paused buffer's head since `Arm()` because
    /// `mRetentionCap` was breached.
    std::atomic<uint64_t> mPausedDropCount{0};

    /// Guards `mPausedBatches`. Worker appends/trims; GUI reads size for
    /// the status bar and drains on Resume.
    mutable std::mutex mPausedMutex;
    std::vector<loglib::StreamedBatch> mPausedBatches;

    /// Bounded SPSC queue between worker `OnBatch` and the GUI drain.
    /// Producer blocks once full; consumer never blocks.
    logapp::BoundedBatchQueue mPending;

    /// True between an `OnBatch` enqueue that posted a `Drain` lambda
    /// and the GUI thread picking it up. Coalesces multiple worker
    /// enqueues onto a single GUI-thread drain lambda per epoch.
    std::atomic<bool> mDrainScheduled{false};

    /// Backs `IsActive()`: set by `Arm()`, cleared by matching
    /// `OnFinished` or `DropPendingBatches()`. GUI thread.
    bool mActive = false;
};
