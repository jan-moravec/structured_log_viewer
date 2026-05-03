#include "loglib/internal/batch_coalescer.hpp"

#include "loglib/key_index.hpp"

#include <string>
#include <utility>

namespace loglib::internal
{

BatchCoalescer::BatchCoalescer(
    LogParseSink &sink, KeyIndex &keys, size_t flushLines, std::chrono::milliseconds flushInterval
) noexcept
    : mSink(sink),
      mKeys(keys),
      mPrevKeyCount(keys.Size()),
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

void BatchCoalescer::DrainNewKeysInto(StreamedBatch &out)
{
    const size_t currentKeyCount = mKeys.Size();
    if (currentKeyCount > mPrevKeyCount)
    {
        out.newKeys.reserve(out.newKeys.size() + (currentKeyCount - mPrevKeyCount));
        for (size_t i = mPrevKeyCount; i < currentKeyCount; ++i)
        {
            out.newKeys.emplace_back(std::string(mKeys.KeyOf(static_cast<KeyId>(i))));
        }
        mPrevKeyCount = currentKeyCount;
    }
}

bool BatchCoalescer::TryFlush(bool force)
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
        // Threshold reached but nothing real to ship -- bump the timer
        // so the next interval restarts and bail out without burdening
        // the sink with an empty batch.
        const bool hasContent =
            !mPending.lines.empty() || !mPending.errors.empty() || mKeys.Size() > mPrevKeyCount;
        if (!hasContent)
        {
            mLastFlush = now;
            return false;
        }
    }

    DrainNewKeysInto(mPending);
    mSink.OnBatch(std::move(mPending));
    mPending = StreamedBatch{};
    mPrimed = false;
    mLastFlush = now;
    return true;
}

void BatchCoalescer::Finish(size_t fallbackLineNumber, bool wasCancelled)
{
    const bool hasContent = mPrimed || mKeys.Size() > mPrevKeyCount || !mPending.errors.empty() ||
                            !mPending.localLineOffsets.empty();
    if (hasContent)
    {
        if (!mPrimed)
        {
            Prime(fallbackLineNumber);
        }
        DrainNewKeysInto(mPending);
        mSink.OnBatch(std::move(mPending));
        mPending = StreamedBatch{};
        mPrimed = false;
    }
    else
    {
        StreamedBatch tail;
        tail.firstLineNumber = fallbackLineNumber;
        DrainNewKeysInto(tail);
        mSink.OnBatch(std::move(tail));
    }
    mSink.OnFinished(wasCancelled);
}

} // namespace loglib::internal
