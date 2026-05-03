#pragma once

#include "loglib/log_parse_sink.hpp"

#include <chrono>
#include <cstddef>

namespace loglib
{
class KeyIndex;
}

namespace loglib::internal
{

/// Shared GUI-batch coalescer used by both the static TBB pipeline and the
/// live-tail streaming loop. Owns:
///   * the in-flight `StreamedBatch` buffer plus its primed-state flag,
///   * the running `KeyIndex` cursor used to diff new keys per batch,
///   * the time-based throttle (last-flush timestamp) that gates the
///     interval-based flush threshold.
///
/// The two pipelines push line / error content into `Pending()` directly
/// and call `TryFlush(false)` after each unit of work to honour the
/// `(flushLines, flushInterval)` thresholds. `Finish` emits one final
/// batch (possibly empty, primed at the caller-supplied fallback line
/// number) and forwards `OnFinished` to the sink, satisfying the sink
/// contract that at least one `OnBatch` precedes every `OnFinished` even
/// on early-exit paths.
class BatchCoalescer
{
public:
    BatchCoalescer(
        LogParseSink &sink, KeyIndex &keys, size_t flushLines, std::chrono::milliseconds flushInterval
    ) noexcept;

    BatchCoalescer(const BatchCoalescer &) = delete;
    BatchCoalescer &operator=(const BatchCoalescer &) = delete;
    BatchCoalescer(BatchCoalescer &&) = delete;
    BatchCoalescer &operator=(BatchCoalescer &&) = delete;

    /// Direct mutation handle on the in-flight batch. Callers append
    /// `lines`, `errors`, and (static path only) `localLineOffsets` here.
    [[nodiscard]] StreamedBatch &Pending() noexcept
    {
        return mPending;
    }

    /// Sets `Pending().firstLineNumber` once. Subsequent calls are no-ops
    /// so the absolute line number of the *first* line in the batch
    /// stays stable as more content lands.
    void Prime(size_t firstLineNumber) noexcept;

    [[nodiscard]] bool IsPrimed() const noexcept
    {
        return mPrimed;
    }

    /// Append the keys added to the index since the last drain into
    /// @p out's `newKeys`, and bump the running cursor. Public so the
    /// static `prefersUncoalesced` branch can drain into a per-batch
    /// scratch `StreamedBatch` that bypasses `Pending()`.
    void DrainNewKeysInto(StreamedBatch &out);

    /// Flush the pending batch to the sink if either threshold is met,
    /// or unconditionally when @p force is true. Returns true if a batch
    /// was emitted. Empty threshold-triggered flushes (no lines, no
    /// errors, no new keys) are skipped to avoid dispatching busy-work
    /// to the GUI thread; `lastFlush` still ticks forward in that case.
    bool TryFlush(bool force);

    /// Emit one final batch and forward `OnFinished(@p wasCancelled)`. If
    /// nothing has been buffered, primes a fresh empty batch at
    /// @p fallbackLineNumber so the sink contract is honoured. Safe to
    /// call from any early-exit path.
    void Finish(size_t fallbackLineNumber, bool wasCancelled);

private:
    LogParseSink &mSink;
    KeyIndex &mKeys;
    StreamedBatch mPending;
    bool mPrimed = false;
    size_t mPrevKeyCount;
    size_t mFlushLines;
    std::chrono::milliseconds mFlushInterval;
    std::chrono::steady_clock::time_point mLastFlush;
};

} // namespace loglib::internal
