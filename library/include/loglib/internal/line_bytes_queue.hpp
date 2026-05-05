#pragma once

#include <atomic>
#include <cstddef>
#include <cstring>
#include <deque>
#include <span>
#include <string_view>

namespace loglib::internal
{

/// Single-producer / single-consumer FIFO byte queue with line-aware
/// back-pressure. Used by `TcpServerProducer` and `UdpServerProducer`
/// to buffer bytes between the Asio worker thread and the parser
/// thread. The queue is *not* internally synchronised; callers wrap
/// access in their own mutex (the same one that already guards the
/// pending state in those producers).
///
/// Back-pressure semantics: when an `Append` would push the queue
/// past `cap` bytes, the oldest queued bytes are dropped down to the
/// next newline boundary so the consumer never sees a torn line. The
/// drop count goes into a caller-supplied atomic so the producer can
/// surface it via its `DroppedByteCount` accessor.
///
/// Backed by `std::deque<char>` for O(1) `pop_front` and bounded
/// resident memory under sustained back-pressure. Read sizes
/// (`STREAMING_READ_BUFFER_SIZE`) stay below a deque block, so most
/// `Read` calls do not cross a block boundary.
class LineBytesQueue
{
public:
    LineBytesQueue() = default;

    LineBytesQueue(const LineBytesQueue &) = delete;
    LineBytesQueue &operator=(const LineBytesQueue &) = delete;
    LineBytesQueue(LineBytesQueue &&) = delete;
    LineBytesQueue &operator=(LineBytesQueue &&) = delete;
    ~LineBytesQueue() = default;

    /// Number of bytes currently queued.
    [[nodiscard]] std::size_t Size() const noexcept
    {
        return mBytes.size();
    }

    /// Whether the queue holds zero bytes.
    [[nodiscard]] bool Empty() const noexcept
    {
        return mBytes.empty();
    }

    /// Copy up to `buffer.size()` bytes into @p buffer and pop them
    /// off the front. Returns the actual byte count copied.
    std::size_t Read(std::span<char> buffer) noexcept
    {
        if (buffer.empty() || mBytes.empty())
        {
            return 0;
        }
        const std::size_t toCopy = std::min(buffer.size(), mBytes.size());
        // `std::copy_n` over a deque iterator is the conventional way
        // to extract a span; modern stdlibs lower it to a memcpy per
        // contiguous block.
        std::copy_n(mBytes.begin(), toCopy, buffer.data());
        mBytes.erase(mBytes.begin(), mBytes.begin() + static_cast<std::ptrdiff_t>(toCopy));
        return toCopy;
    }

    /// Append @p data of length @p size, applying line-aware
    /// back-pressure: if the resulting queue would exceed @p cap, drop
    /// oldest bytes (rounded up to the next newline) until it fits.
    /// `cap == 0` disables back-pressure. Increments @p droppedCounter
    /// by the number of bytes dropped.
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

            // Round drop up to the next newline so the consumer never
            // sees a torn line. If no newline within the drop region,
            // extend through the rest of the buffered bytes (extreme
            // back-pressure: the parser is hopelessly behind).
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

    /// Convenience overload for `std::string_view` payloads.
    void Append(std::string_view payload, std::size_t cap, std::atomic<std::size_t> &droppedCounter)
    {
        Append(payload.data(), payload.size(), cap, droppedCounter);
    }

private:
    std::deque<char> mBytes;
};

} // namespace loglib::internal
