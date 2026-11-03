/**
 * @brief Byte queue that drops whole lines under back-pressure.
 *
 * The TCP and UDP producers buffer bytes in this queue between the network thread and the parser thread. When the queue
 * grows past its limit it discards the oldest bytes up to the next newline, so the reader never sees a torn line, and
 * it counts the dropped bytes. The queue is not thread-safe; callers lock around it.
 *
 * This is part of the ingest module.
 */
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <deque>
#include <span>
#include <string_view>

namespace loglib::internal
{

/**
 * @brief A first-in first-out byte queue that drops whole lines when it is full.
 *
 * One thread appends and one thread reads. The queue does no locking, so the producer locks around it with the mutex
 * that already guards its other state.
 *
 * When an Append would take the queue past its limit, the oldest bytes are dropped up to the next newline, so a reader
 * never sees half a line. The number of dropped bytes is added to a counter the caller passes in, which the producer
 * reports as its DroppedByteCount.
 *
 * It is backed by a deque, which removes bytes at the front in constant time and keeps memory bounded while the
 * limit is enforced.
 */
class LineBytesQueue
{
public:
    LineBytesQueue() = default;

    LineBytesQueue(const LineBytesQueue &) = delete;
    LineBytesQueue &operator=(const LineBytesQueue &) = delete;
    LineBytesQueue(LineBytesQueue &&) = delete;
    LineBytesQueue &operator=(LineBytesQueue &&) = delete;
    ~LineBytesQueue() = default;

    /** @brief The number of bytes in the queue. */
    [[nodiscard]] std::size_t Size() const noexcept
    {
        return mBytes.size();
    }

    [[nodiscard]] bool Empty() const noexcept
    {
        return mBytes.empty();
    }

    /**
     * @brief Moves up to buffer.size() bytes from the front of the queue into buffer and returns how many were moved.
     */
    std::size_t Read(std::span<char> buffer) noexcept
    {
        if (buffer.empty() || mBytes.empty())
        {
            return 0;
        }
        const std::size_t toCopy = std::min(buffer.size(), mBytes.size());
        std::copy_n(mBytes.begin(), toCopy, buffer.data());
        mBytes.erase(mBytes.begin(), mBytes.begin() + static_cast<std::ptrdiff_t>(toCopy));
        return toCopy;
    }

    /**
     * @brief Adds bytes to the end of the queue.
     *
     * If the queue would grow past cap, the oldest bytes are dropped first, up to and including the next newline, until
     * the new bytes fit. If there is no newline in what is dropped, everything queued is dropped. A cap of zero turns
     * the limit off. The number of dropped bytes is added to droppedCounter.
     */
    void Append(const char *data, std::size_t size, std::size_t cap, std::atomic<std::size_t> &droppedCounter)
    {
        if (size == 0)
        {
            return;
        }
        if (cap > 0 && mBytes.size() + size > cap)
        {
            const std::size_t over = mBytes.size() + size - cap;
            const std::size_t dropFrom = std::min(over, mBytes.size());

            // Extend the drop to the next newline so the reader never sees a torn line.
            std::size_t dropCount = dropFrom;
            while (dropCount < mBytes.size() && mBytes[dropCount] != '\n')
            {
                ++dropCount;
            }
            if (dropCount < mBytes.size() && mBytes[dropCount] == '\n')
            {
                ++dropCount; // include the newline
            }
            mBytes.erase(mBytes.begin(), mBytes.begin() + static_cast<std::ptrdiff_t>(dropCount));
            droppedCounter.fetch_add(dropCount, std::memory_order_acq_rel);
        }
        mBytes.insert(mBytes.end(), data, data + size);
    }

    /** @brief Same as the pointer overload, for a string_view. */
    void Append(std::string_view payload, std::size_t cap, std::atomic<std::size_t> &droppedCounter)
    {
        Append(payload.data(), payload.size(), cap, droppedCounter);
    }

private:
    std::deque<char> mBytes;
};

} // namespace loglib::internal
