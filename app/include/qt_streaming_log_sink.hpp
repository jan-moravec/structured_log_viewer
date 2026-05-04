#pragma once

#include <loglib/stop_token.hpp>
#include <loglib/log_parse_sink.hpp>

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
/// the GUI thread; worker callbacks post via QueuedConnection and drop on
/// generation mismatch.
///
/// Live-tail Pause/Resume: while paused the worker buffers parsed batches
/// internally instead of posting them; `Resume()` coalesces the buffer
/// and posts it once. The buffer is bounded by the retention cap so an
/// indefinitely-paused noisy producer cannot OOM.
class QtStreamingLogSink : public QObject, public loglib::LogParseSink
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

    /// The canonical KeyIndex (the model's LogTable's). Thread-safe.
    loglib::KeyIndex &Keys() override;

    void OnStarted() override;
    void OnBatch(loglib::StreamedBatch batch) override;
    void OnFinished(bool cancelled) override;

private:
    /// Concatenates the batches into one. The first batch's
    /// `firstLineNumber` is preserved; all other vectors are appended.
    /// Used by `Resume()` and `TakePausedBuffer()`.
    static loglib::StreamedBatch CoalesceLocked(std::vector<loglib::StreamedBatch> &&batches);

    /// Total row count across `mPausedBatches`. Caller must hold
    /// `mPausedMutex`.
    size_t PausedLineCountLocked() const;

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

    /// Backs `IsActive()`: set by `Arm()`, cleared by matching
    /// `OnFinished` or `DropPendingBatches()`. GUI thread.
    bool mActive = false;
};
