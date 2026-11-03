/**
 * @brief Shared state and reader-side methods of the queue-backed network producers.
 *
 * The TCP and UDP producers collect bytes on a network thread and hand them to a reader through a locked byte queue.
 * The reader-side calls (read, wait, closed check, display name) and the status callback behave the same for both, so
 * they live here. A producer derives from this class, fills the queue under the lock, and sets the stop and
 * worker-exited flags.
 *
 * This is part of the ingest module.
 */
#pragma once

#include "loglib/core/bytes_producer.hpp"
#include "loglib/ingest/internal/line_bytes_queue.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <span>
#include <string>

namespace loglib::internal
{

class QueuedProducerState
{
public:
    QueuedProducerState() = default;
    QueuedProducerState(const QueuedProducerState &) = delete;
    QueuedProducerState &operator=(const QueuedProducerState &) = delete;
    QueuedProducerState(QueuedProducerState &&) = delete;
    QueuedProducerState &operator=(QueuedProducerState &&) = delete;

    /** @brief Moves up to buffer.size() queued bytes into buffer and returns how many were moved. */
    size_t Read(std::span<char> buffer)
    {
        if (buffer.empty())
        {
            return 0;
        }
        const std::scoped_lock lock(mMutex);
        return mReadyBuffer.Read(buffer);
    }

    /**
     * @brief Blocks until bytes are queued, the producer stops, or the timeout passes. A zero timeout returns at once.
     */
    void WaitForBytes(std::chrono::milliseconds timeout)
    {
        if (timeout.count() <= 0)
        {
            return;
        }
        std::unique_lock<std::mutex> lock(mMutex);
        mCv.wait_for(lock, timeout, [&] {
            return !mReadyBuffer.Empty() || mStopRequested.load(std::memory_order_acquire) ||
                   mWorkerExited.load(std::memory_order_acquire);
        });
    }

    /** @brief True once the network thread has exited and every queued byte has been read. */
    [[nodiscard]] bool IsClosed() const noexcept
    {
        if (!mWorkerExited.load(std::memory_order_acquire))
        {
            return false;
        }
        const std::scoped_lock lock(mMutex);
        return mReadyBuffer.Empty();
    }

    [[nodiscard]] std::string DisplayName() const
    {
        return mDisplayName;
    }

    /** @brief Installs the status callback and immediately reports the current status to it. */
    void SetStatusCallback(const std::function<void(SourceStatus)> &callback)
    {
        std::function<void(SourceStatus)> snapshot;
        SourceStatus current{};
        {
            const std::scoped_lock lock(mCallbackMutex);
            mStatusCallback = callback;
            snapshot = mStatusCallback;
            current = mLastReportedStatus;
        }
        if (snapshot)
        {
            snapshot(current);
        }
    }

protected:
    ~QueuedProducerState() = default;

    /** @brief Moves the status from Waiting to Running once, on the first real traffic, and notifies the callback. */
    void MarkRunning()
    {
        std::function<void(SourceStatus)> callback;
        {
            const std::scoped_lock lock(mCallbackMutex);
            if (mLastReportedStatus == SourceStatus::Running)
            {
                return;
            }
            mLastReportedStatus = SourceStatus::Running;
            callback = mStatusCallback;
        }
        if (callback)
        {
            callback(SourceStatus::Running);
        }
    }

    // The derived producers lock, wait on and fill this state directly from their network threads, so the members
    // are protected rather than private.
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes,cppcoreguidelines-non-private-member-variables-in-classes)
    std::string mDisplayName;

    mutable std::mutex mMutex;
    std::condition_variable mCv;
    LineBytesQueue mReadyBuffer;

    std::atomic<bool> mStopRequested{false};
    std::atomic<bool> mWorkerExited{false};

    mutable std::mutex mCallbackMutex;
    std::function<void(SourceStatus)> mStatusCallback;
    SourceStatus mLastReportedStatus = SourceStatus::Waiting;
    // NOLINTEND(misc-non-private-member-variables-in-classes,cppcoreguidelines-non-private-member-variables-in-classes)
};

} // namespace loglib::internal
