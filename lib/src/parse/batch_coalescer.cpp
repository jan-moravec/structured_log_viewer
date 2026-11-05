#include "loglib/parse/internal/batch_coalescer.hpp"

#include "loglib/core/key_index.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace loglib::internal
{

BatchCoalescer::BatchCoalescer(
    LogParseSink &sink, KeyIndex &keys, size_t flushLines, std::chrono::milliseconds flushInterval
) noexcept
    : mSink(sink),
      mKeys(keys),
      mFlushLines(flushLines),
      mFlushInterval(flushInterval),
      mLastFlush(std::chrono::steady_clock::now())
{
}

void BatchCoalescer::Prime(size_t firstLineNumber) noexcept
{
    if (!mPrimed)
    {
        mPending.firstLineNumber = firstLineNumber;
        mPrimed = true;
    }
}

void BatchCoalescer::AddSeenKeys(std::span<const KeyId> ids)
{
    for (const KeyId id : ids)
    {
        if (id >= mAnnounced.size())
        {
            mAnnounced.resize(std::max<size_t>(static_cast<size_t>(id) + 1, mAnnounced.size() * 2), false);
        }
        if (!mAnnounced[id])
        {
            mAnnounced[id] = true;
            mPending.newKeys.emplace_back(mKeys.KeyOf(id));
        }
    }
}

bool BatchCoalescer::TryFlush(bool force, size_t lastLineNumber)
{
    const auto now = std::chrono::steady_clock::now();
    if (!force)
    {
        const bool sizeReached = mPending.lines.size() >= mFlushLines;
        const bool intervalReached = (now - mLastFlush) >= mFlushInterval;
        if (!sizeReached && !intervalReached)
        {
            return false;
        }
        // Threshold reached but nothing to ship: tick the timer and
        // skip rather than burden the sink with an empty batch.
        const bool hasContent = !mPending.lines.empty() || !mPending.errors.empty() || !mPending.newKeys.empty();
        if (!hasContent)
        {
            mLastFlush = now;
            return false;
        }
    }

    mPending.lastLineNumber = lastLineNumber;
    mPending.convertedTimeKeys = mConvertedTimeKeys;
    mSink.OnBatch(std::move(mPending));
    mPending = RowBatch{};
    mPrimed = false;
    mLastFlush = now;
    return true;
}

void BatchCoalescer::Finish(size_t lastLineNumber, bool wasCancelled)
{
    const bool hasContent =
        mPrimed || !mPending.newKeys.empty() || !mPending.errors.empty() || !mPending.localLineOffsets.empty();
    if (hasContent)
    {
        if (!mPrimed)
        {
            Prime(lastLineNumber + 1);
        }
        mPending.lastLineNumber = lastLineNumber;
        mPending.convertedTimeKeys = mConvertedTimeKeys;
        mSink.OnBatch(std::move(mPending));
        mPending = RowBatch{};
        mPrimed = false;
    }
    else
    {
        RowBatch tail;
        tail.firstLineNumber = lastLineNumber + 1;
        tail.lastLineNumber = lastLineNumber;
        tail.convertedTimeKeys = mConvertedTimeKeys;
        mSink.OnBatch(std::move(tail));
    }
    mSink.OnFinished(wasCancelled);
}

} // namespace loglib::internal
