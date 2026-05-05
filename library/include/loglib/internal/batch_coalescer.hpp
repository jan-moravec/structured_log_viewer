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

/// Shared GUI-batch coalescer for both the static TBB pipeline and the
/// live-tail streaming loop. Holds the in-flight `StreamedBatch`, the
/// `KeyIndex` cursor for per-batch new-key diffs, and the
/// (flushLines, flushInterval) throttle. Pipelines push content into
/// `Pending()` and call `TryFlush(false)` to flush on threshold.
/// `Finish` always emits a final batch — possibly empty — and forwards
/// `OnFinished`, so the sink contract holds even on early-exit paths.
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

    /// In-flight batch. Callers append `lines`, `errors`, and
    /// (static path only) `localLineOffsets` directly.
    [[nodiscard]] StreamedBatch &Pending() noexcept
    {
        return mPending;
    }

    /// Sets `Pending().firstLineNumber` once; subsequent calls are
    /// no-ops so the line number stays anchored to the first line.
    void Prime(size_t firstLineNumber) noexcept;

    [[nodiscard]] bool IsPrimed() const noexcept
    {
        return mPrimed;
    }

    /// Append the keys added since the last drain into @p out and
    /// advance the cursor. Public so the static `prefersUncoalesced`
    /// branch can drain into a per-batch scratch batch.
    void DrainNewKeysInto(StreamedBatch &out);

    /// Flush if either threshold is met, or unconditionally if
    /// @p force. Returns true if a batch was emitted. Empty
    /// threshold-triggered flushes are skipped (but `lastFlush` ticks).
    bool TryFlush(bool force);

    /// Emit a final batch (empty if nothing buffered, primed at
    /// @p fallbackLineNumber) and forward `OnFinished(@p wasCancelled)`.
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
