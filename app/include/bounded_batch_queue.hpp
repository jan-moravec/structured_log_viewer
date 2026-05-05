#pragma once

#include <loglib/log_parse_sink.hpp>

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <iterator>
#include <mutex>
#include <utility>
#include <vector>

namespace logapp
{

/// Bounded SPSC queue between the parser worker and the GUI thread.
///
/// Producer (worker) blocks in `WaitEnqueue` once the queue holds
/// `capacity` items, propagating GUI back-pressure all the way back to
/// the TBB flow graph (Stage A blocks once Stage C blocks once the
/// sink blocks here). Consumer (GUI) never blocks; it pulls everything
/// pending in one shot via `DrainAll` after a `QMetaObject::invokeMethod`
/// post wakes it.
///
/// Cancellation is explicit: `NotifyStop` flips a flag and broadcasts
/// to wake any blocked producer immediately. We do not use
/// `std::stop_token` because `loglib::StopToken` has no callback
/// registry (see `loglib/stop_token.hpp`). `Reset` re-arms the queue
/// for a new session and is intended to be called from the GUI thread
/// after the previous worker has been joined.
class BoundedBatchQueue
{
public:
    explicit BoundedBatchQueue(std::size_t capacity)
        : mCapacity(capacity == 0 ? 1 : capacity)
    {
    }

    BoundedBatchQueue(const BoundedBatchQueue &) = delete;
    BoundedBatchQueue &operator=(const BoundedBatchQueue &) = delete;
    BoundedBatchQueue(BoundedBatchQueue &&) = delete;
    BoundedBatchQueue &operator=(BoundedBatchQueue &&) = delete;
    ~BoundedBatchQueue() = default;

    /// Producer-thread. Blocks until a slot opens or `NotifyStop` fires.
    /// Returns `false` only if the stop happens while we were blocked
    /// waiting for a slot; the batch is then discarded. If a slot is
    /// available we always enqueue, even after stop -- this preserves
    /// the existing sink contract that drain-phase `OnBatch` calls
    /// (between `RequestStop` and the worker join) deliver normally.
    bool WaitEnqueue(loglib::StreamedBatch batch)
    {
        std::unique_lock<std::mutex> lock(mMtx);
        mNotFull.wait(lock, [this] { return mStopped || mItems.size() < mCapacity; });
        if (mItems.size() < mCapacity)
        {
            mItems.push_back(std::move(batch));
            return true;
        }
        // Stopped while the queue was full -- drop the batch.
        return false;
    }

    /// Consumer-thread. Moves every currently-buffered batch out and
    /// wakes one blocked producer (if any).
    [[nodiscard]] std::vector<loglib::StreamedBatch> DrainAll()
    {
        std::vector<loglib::StreamedBatch> out;
        {
            std::scoped_lock<std::mutex> lock(mMtx);
            out.reserve(mItems.size());
            std::move(mItems.begin(), mItems.end(), std::back_inserter(out));
            mItems.clear();
        }
        mNotFull.notify_one();
        return out;
    }

    /// Idempotent. Sets the stopped flag and broadcasts to wake any
    /// blocked producer; subsequent `WaitEnqueue` calls return `false`
    /// until `Reset` is called.
    void NotifyStop()
    {
        {
            std::scoped_lock<std::mutex> lock(mMtx);
            mStopped = true;
        }
        mNotFull.notify_all();
    }

    /// GUI-thread, intended to be called from `Arm()` after the
    /// previous worker has been joined: clears any leftover items and
    /// re-arms the queue (clears the stopped flag) for a new session.
    void Reset()
    {
        std::scoped_lock<std::mutex> lock(mMtx);
        mItems.clear();
        mStopped = false;
    }

    [[nodiscard]] std::size_t SizeApprox() const
    {
        std::scoped_lock<std::mutex> lock(mMtx);
        return mItems.size();
    }

    [[nodiscard]] std::size_t Capacity() const noexcept
    {
        return mCapacity;
    }

private:
    mutable std::mutex mMtx;
    std::condition_variable mNotFull;
    std::deque<loglib::StreamedBatch> mItems;
    std::size_t mCapacity;
    bool mStopped = false;
};

} // namespace logapp
