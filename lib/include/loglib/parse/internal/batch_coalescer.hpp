/**
 * @brief Groups parsed lines into batches for the sink.
 *
 * Sending one batch per line would swamp the sink, so the coalescer collects lines and flushes them when enough have
 * built up or enough time has passed. It also records which keys are new in each batch and always sends a final batch
 * before finishing. Both the static pipeline and the live-stream loop use it.
 *
 * This is part of the parse module.
 */
#pragma once

#include "loglib/parse/log_parse_sink.hpp"

#include <chrono>
#include <cstddef>
#include <optional>

namespace loglib
{
class KeyIndex;
}

namespace loglib::internal
{

/**
 * @brief Collects parsed lines into batches and sends them to a sink when enough have piled up or enough time has
 * passed.
 *
 * Both the static pipeline and the streaming loop use it. It holds the batch being built and remembers how many keys
 * the sink has already been told about, so each batch can list only the new keys. A pipeline adds content through
 * Pending and calls TryFlush with false to send a batch when a threshold is reached. Finish always sends a last batch,
 * which can be empty, and then calls OnFinished, so the sink gets its usual calls even when a parse ends early.
 */
class BatchCoalescer
{
public:
    /**
     * @param newKeyBaseline The key count to start counting new keys from. By default it is the current size of the
     * index. A parser that adds keys before the pipeline starts, such as the CSV parser with its header, passes the
     * count from before, so those columns still appear as new keys.
     */
    BatchCoalescer(
        LogParseSink &sink,
        KeyIndex &keys,
        size_t flushLines,
        std::chrono::milliseconds flushInterval,
        std::optional<size_t> newKeyBaseline = std::nullopt
    ) noexcept;

    BatchCoalescer(const BatchCoalescer &) = delete;
    BatchCoalescer &operator=(const BatchCoalescer &) = delete;
    BatchCoalescer(BatchCoalescer &&) = delete;
    BatchCoalescer &operator=(BatchCoalescer &&) = delete;
    ~BatchCoalescer() = default;

    /** @brief The batch being built. Callers add lines and errors to it, and the static path also adds line offsets. */
    [[nodiscard]] StreamedBatch &Pending() noexcept
    {
        return mPending;
    }

    /**
     * @brief Sets the first line number of the pending batch. Only the first call has an effect, so the number stays
     * that of the first line.
     */
    void Prime(size_t firstLineNumber) noexcept;

    [[nodiscard]] bool IsPrimed() const noexcept
    {
        return mPrimed;
    }

    /** @brief Appends the keys added since the last call to out, and remembers that they were reported. */
    void DrainNewKeysInto(StreamedBatch &out);

    /**
     * @brief Sends the pending batch if a threshold was reached, or always when force is set. Returns true if a batch
     * was sent.
     *
     * An empty batch is not sent for a threshold, but the time of the last flush is still updated.
     */
    bool TryFlush(bool force);

    /**
     * @brief Sends a last batch, which is empty if nothing is pending, and then calls OnFinished with wasCancelled. An
     * empty batch gets fallbackLineNumber as its first line.
     */
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
