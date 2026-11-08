/**
 * @brief Groups parsed lines into batches for the sink.
 *
 * Sending one batch per line would swamp the sink, so the coalescer collects lines and flushes them when enough have
 * built up or enough time has passed. It also collects the keys the batch announces and always sends a final batch
 * before finishing. Both the static pipeline and the live-stream loop use it.
 *
 * This is part of the parse module.
 */
#pragma once

#include "loglib/core/key_index.hpp"
#include "loglib/parse/log_parse_sink.hpp"

#include <fmt/format.h>

#include <chrono>
#include <cstddef>
#include <exception>
#include <span>
#include <utility>
#include <vector>

namespace loglib::internal
{

/**
 * @brief The error for a run of continuation lines that has no record to join. Both parse drivers report one such error
 * per run, with the first and last line of the run.
 */
inline ParseError MakeOrphanedContinuationError(size_t firstLine, size_t lastLine)
{
    if (firstLine == lastLine)
    {
        return ParseError{.firstLine = firstLine, .lastLine = lastLine, .message = "Orphaned continuation line."};
    }
    return ParseError{
        .firstLine = firstLine,
        .lastLine = lastLine,
        .message = fmt::format("{} orphaned continuation lines dropped.", lastLine - firstLine + 1),
    };
}

/**
 * @brief Collects parsed lines into batches and sends them to a sink when enough have piled up or enough time has
 * passed.
 *
 * Both the static pipeline and the streaming loop use it. It holds the batch being built and remembers which keys the
 * sink has been told about, so each key is announced once, in the batch where it first came up. The caller says which
 * keys a batch holds through AddSeenKeys, in the order they first appear in the input. A pipeline adds content through
 * Pending and calls TryFlush with false to send a batch when a threshold is reached. Finish always sends a last batch,
 * which can be empty, and then calls OnFinished, so the sink gets its usual calls even when a parse ends early.
 */
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
    ~BatchCoalescer() = default;

    /** @brief The batch being built. Callers add lines and errors to it, and the static path also adds line offsets. */
    [[nodiscard]] RowBatch &Pending() noexcept
    {
        return mPending;
    }

    /**
     * @brief Sets the first line number of the pending batch. Only the first call has an effect, so the number stays
     * that of the first line.
     */
    void Prime(size_t firstLineNumber) noexcept;

    /** @brief Sets the converted time keys that every batch carries. Call it once, before the first flush. */
    void SetConvertedTimeKeys(std::vector<KeyId> keys)
    {
        mConvertedTimeKeys = std::move(keys);
    }

    [[nodiscard]] bool IsPrimed() const noexcept
    {
        return mPrimed;
    }

    /**
     * @brief Announces keys in the pending batch, in the order given. A key that an earlier call announced is skipped.
     * The ids must belong to the key index of the sink.
     */
    void AddSeenKeys(std::span<const KeyId> ids);

    /**
     * @brief Sends the pending batch if a threshold was reached, or always when force is set. Returns true if a batch
     * was sent.
     *
     * lastLineNumber is the number of the last physical line the caller has read, and becomes the batch's
     * lastLineNumber. An empty batch is not sent for a threshold, but the time of the last flush is still updated.
     */
    bool TryFlush(bool force, size_t lastLineNumber);

    /**
     * @brief Sends a last batch, which is empty if nothing is pending, and then calls OnFinished with wasCancelled.
     *
     * lastLineNumber is the total number of physical lines read. An empty batch starts at the line after it.
     */
    void Finish(size_t lastLineNumber, bool wasCancelled);

    /**
     * @brief Keeps the sink contract when a parse threw. Call it from the handler of the exception, then rethrow.
     *
     * For OperationCancelled it finishes the sink as cancelled. For any other exception it adds one error without a
     * line, "Streaming parse failed: " and the message, to the last batch and finishes the sink. It does nothing when
     * the sink itself threw, because a sink that threw is not called again, or when the sink was finished already.
     * lastLineNumber is the number of lines read so far.
     */
    void FinishAfterException(size_t lastLineNumber, std::exception_ptr error);

private:
    /** Sends a batch to the sink and remembers whether the sink threw. */
    void SendBatch(RowBatch batch);

    LogParseSink &mSink;
    KeyIndex &mKeys;
    RowBatch mPending;
    bool mPrimed = false;
    /** One bit per key id: set once the key is in the newKeys of some batch. */
    std::vector<bool> mAnnounced;
    bool mSinkThrew = false;
    bool mFinished = false;
    std::vector<KeyId> mConvertedTimeKeys;
    size_t mFlushLines;
    std::chrono::milliseconds mFlushInterval;
    std::chrono::steady_clock::time_point mLastFlush;
};

} // namespace loglib::internal
