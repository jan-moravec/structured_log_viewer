#include "loglib/udp_server_producer.hpp"

#include <asio.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace loglib::internal
{

class UdpServerProducerImpl
{
public:
    explicit UdpServerProducerImpl(UdpServerProducer::Options options);
    ~UdpServerProducerImpl();

    UdpServerProducerImpl(const UdpServerProducerImpl &) = delete;
    UdpServerProducerImpl &operator=(const UdpServerProducerImpl &) = delete;
    UdpServerProducerImpl(UdpServerProducerImpl &&) = delete;
    UdpServerProducerImpl &operator=(UdpServerProducerImpl &&) = delete;

    size_t Read(std::span<char> buffer);
    void WaitForBytes(std::chrono::milliseconds timeout);
    void Stop() noexcept;
    [[nodiscard]] bool IsClosed() const noexcept;
    [[nodiscard]] std::string DisplayName() const;
    void SetStatusCallback(std::function<void(SourceStatus)> callback);
    [[nodiscard]] uint16_t BoundPort() const noexcept;
    [[nodiscard]] size_t DatagramCount() const noexcept;
    [[nodiscard]] size_t DroppedByteCount() const noexcept;

private:
    /// Posts the next `async_receive_from` and chains the completion
    /// handler back to itself. Runs on the I/O worker thread.
    void StartReceive();

    /// Append @p bytes to the byte queue, normalising the trailing
    /// newline. Drops front bytes (under-the-hood FIFO) if the queue
    /// would exceed `mOptions.maxQueueBytes`.
    void AppendDatagramLocked(const char *data, size_t size);

    UdpServerProducer::Options mOptions;
    std::string mDisplayName;

    asio::io_context mIoContext;
    asio::ip::udp::socket mSocket;

    /// Reused per receive. Sized to `mOptions.maxDatagramBytes` so
    /// `async_receive_from` writes directly into the final storage.
    std::vector<char> mRecvBuffer;
    asio::ip::udp::endpoint mPeerEndpoint;

    mutable std::mutex mMutex;
    std::condition_variable mCv;

    /// Byte queue (FIFO). `mReadyConsumed` tracks how much of the
    /// front we've already handed to `Read`; we periodically compact
    /// once consumption exceeds half the buffer. Same shape as
    /// `TailingBytesProducer` so the parse loop's `Read`/`WaitForBytes`
    /// pattern stays uniform.
    std::vector<char> mReadyBuffer;
    size_t mReadyConsumed = 0;

    std::atomic<bool> mStopRequested{false};
    std::atomic<bool> mWorkerExited{false};
    std::atomic<size_t> mDatagramCount{0};
    std::atomic<size_t> mDroppedByteCount{0};

    uint16_t mBoundPort = 0;

    /// Status callback bookkeeping. Edge-triggered. UDP has no
    /// "connection state" to track, so we report `Running` once the
    /// socket is open and never flip back; we keep the field anyway
    /// to satisfy the `BytesProducer` contract uniformly.
    mutable std::mutex mCallbackMutex;
    std::function<void(SourceStatus)> mStatusCallback;

    std::thread mWorker;
};

UdpServerProducerImpl::UdpServerProducerImpl(UdpServerProducer::Options options)
    : mOptions(std::move(options)), mSocket(mIoContext), mRecvBuffer(mOptions.maxDatagramBytes)
{
    asio::error_code ec;
    const asio::ip::address bindAddr = asio::ip::make_address(mOptions.bindAddress, ec);
    if (ec)
    {
        throw std::system_error(ec, "UdpServerProducer: invalid bind address '" + mOptions.bindAddress + "'");
    }

    const asio::ip::udp::endpoint endpoint(bindAddr, mOptions.port);
    mSocket.open(endpoint.protocol(), ec);
    if (ec)
    {
        throw std::system_error(ec, "UdpServerProducer: socket open failed");
    }
    // SO_REUSEADDR is friendly for tests that bind ephemeral ports
    // back-to-back; the OS still rejects truly conflicting binds via
    // SO_EXCLUSIVEADDRUSE on Windows or the default semantics on POSIX.
    mSocket.set_option(asio::socket_base::reuse_address(true), ec);
    mSocket.bind(endpoint, ec);
    if (ec)
    {
        throw std::system_error(
            ec, "UdpServerProducer: bind failed for " + mOptions.bindAddress + ":" + std::to_string(mOptions.port)
        );
    }

    mBoundPort = mSocket.local_endpoint().port();
    mDisplayName = "udp://" + mOptions.bindAddress + ":" + std::to_string(mBoundPort);

    StartReceive();
    mWorker = std::thread([this] {
        // `run()` returns when no work is left; we keep work alive by
        // re-arming the receive in the completion handler. On `Stop()`
        // we close the socket (cancels in-flight ops) and call
        // `mIoContext.stop()` to break out cleanly.
        try
        {
            mIoContext.run();
        }
        catch (...)
        {
            // Asio handlers in this TU never throw, but defensive: if
            // a future change does, we want the worker to exit
            // gracefully rather than terminate the process.
        }
        mWorkerExited.store(true, std::memory_order_release);
        mCv.notify_all();
    });
}

UdpServerProducerImpl::~UdpServerProducerImpl()
{
    Stop();
    if (mWorker.joinable())
    {
        mWorker.join();
    }
}

size_t UdpServerProducerImpl::Read(std::span<char> buffer)
{
    if (buffer.empty())
    {
        return 0;
    }

    std::lock_guard<std::mutex> lock(mMutex);
    const size_t available = mReadyBuffer.size() - mReadyConsumed;
    if (available == 0)
    {
        return 0;
    }
    const size_t toCopy = std::min(available, buffer.size());
    std::memcpy(buffer.data(), mReadyBuffer.data() + mReadyConsumed, toCopy);
    mReadyConsumed += toCopy;

    // Compact when we've consumed >= half the buffer to keep memory
    // bounded for long-running streams.
    if (mReadyConsumed * 2 >= mReadyBuffer.size() && mReadyConsumed > 0)
    {
        mReadyBuffer.erase(mReadyBuffer.begin(), mReadyBuffer.begin() + static_cast<std::ptrdiff_t>(mReadyConsumed));
        mReadyConsumed = 0;
    }
    return toCopy;
}

void UdpServerProducerImpl::WaitForBytes(std::chrono::milliseconds timeout)
{
    if (timeout.count() <= 0)
    {
        return; // non-blocking
    }
    std::unique_lock<std::mutex> lock(mMutex);
    mCv.wait_for(lock, timeout, [&] {
        return (mReadyBuffer.size() - mReadyConsumed) > 0 || mStopRequested.load(std::memory_order_acquire) ||
               mWorkerExited.load(std::memory_order_acquire);
    });
}

void UdpServerProducerImpl::Stop() noexcept
{
    if (mStopRequested.exchange(true, std::memory_order_acq_rel))
    {
        return;
    }

    // Close the socket from the worker thread to avoid races between
    // the running `async_receive_from` and the destructor. Posting to
    // `io_context` is safe from any thread.
    asio::post(mIoContext, [this] {
        asio::error_code ec;
        mSocket.close(ec);
        // Closing cancels the in-flight receive with
        // `operation_aborted`; the completion handler observes
        // `mStopRequested` and stops re-arming.
    });
    mIoContext.stop();
    mCv.notify_all();
}

bool UdpServerProducerImpl::IsClosed() const noexcept
{
    if (!mWorkerExited.load(std::memory_order_acquire))
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(mMutex);
    return (mReadyBuffer.size() - mReadyConsumed) == 0;
}

std::string UdpServerProducerImpl::DisplayName() const
{
    return mDisplayName;
}

void UdpServerProducerImpl::SetStatusCallback(std::function<void(SourceStatus)> callback)
{
    std::function<void(SourceStatus)> snapshot;
    {
        std::lock_guard<std::mutex> lock(mCallbackMutex);
        mStatusCallback = std::move(callback);
        snapshot = mStatusCallback;
    }
    // Fire once with the current state so the GUI gets a fresh
    // edge-triggered baseline. We always report `Running` for UDP --
    // the socket is bound and ready to receive even when no datagrams
    // have arrived yet.
    if (snapshot)
    {
        snapshot(SourceStatus::Running);
    }
}

uint16_t UdpServerProducerImpl::BoundPort() const noexcept
{
    return mBoundPort;
}

size_t UdpServerProducerImpl::DatagramCount() const noexcept
{
    return mDatagramCount.load(std::memory_order_acquire);
}

size_t UdpServerProducerImpl::DroppedByteCount() const noexcept
{
    return mDroppedByteCount.load(std::memory_order_acquire);
}

void UdpServerProducerImpl::StartReceive()
{
    if (mStopRequested.load(std::memory_order_acquire))
    {
        return;
    }
    mSocket.async_receive_from(
        asio::buffer(mRecvBuffer.data(), mRecvBuffer.size()),
        mPeerEndpoint,
        [this](const asio::error_code &ec, std::size_t bytes) {
            if (mStopRequested.load(std::memory_order_acquire))
            {
                return;
            }
            if (ec)
            {
                // EOF semantics for UDP: most "errors" are benign
                // (operation_aborted on close, ICMP-derived
                // connection_refused on Windows). Re-arm unless we're
                // shutting down -- the producer keeps running through
                // transient errors so a single corrupt sender cannot
                // wedge the stream.
                if (ec != asio::error::operation_aborted)
                {
                    StartReceive();
                }
                return;
            }
            if (bytes > 0)
            {
                {
                    std::lock_guard<std::mutex> lock(mMutex);
                    AppendDatagramLocked(mRecvBuffer.data(), bytes);
                }
                mDatagramCount.fetch_add(1, std::memory_order_acq_rel);
                mCv.notify_all();
            }
            StartReceive();
        }
    );
}

void UdpServerProducerImpl::AppendDatagramLocked(const char *data, size_t size)
{
    if (size == 0)
    {
        return;
    }
    const bool needsNewline = data[size - 1] != '\n';

    // Back-pressure: drop oldest queued bytes (after `mReadyConsumed`)
    // if appending this datagram would exceed the cap. We never drop
    // already-in-flight (post-consumed) bytes, only un-read ones, so
    // the consumer never sees a torn line: the FIFO drop happens at
    // the line-boundary granularity that the parse loop consumes at.
    const size_t willAdd = size + (needsNewline ? 1 : 0);
    const size_t cap = mOptions.maxQueueBytes;
    if (cap > 0)
    {
        const size_t pendingNow = mReadyBuffer.size() - mReadyConsumed;
        if (pendingNow + willAdd > cap)
        {
            // Drop the oldest unread bytes until we fit, but never
            // drop more than what's currently pending.
            const size_t over = pendingNow + willAdd - cap;
            const size_t dropFrom = std::min(over, pendingNow);

            // Round drop up to the next newline so the consumer
            // doesn't get a torn line. If no newline within the
            // pending region, drop everything (extreme back-pressure;
            // the parser is hopelessly behind).
            size_t dropTo = mReadyConsumed + dropFrom;
            const size_t pendingEnd = mReadyBuffer.size();
            while (dropTo < pendingEnd && mReadyBuffer[dropTo] != '\n')
            {
                ++dropTo;
            }
            if (dropTo < pendingEnd && mReadyBuffer[dropTo] == '\n')
            {
                ++dropTo; // include the newline so the next byte is a fresh line start
            }

            const size_t actualDrop = dropTo - mReadyConsumed;
            mDroppedByteCount.fetch_add(actualDrop, std::memory_order_acq_rel);
            mReadyConsumed = dropTo;

            // Compact eagerly under back-pressure to free memory.
            if (mReadyConsumed > 0)
            {
                mReadyBuffer.erase(
                    mReadyBuffer.begin(), mReadyBuffer.begin() + static_cast<std::ptrdiff_t>(mReadyConsumed)
                );
                mReadyConsumed = 0;
            }
        }
    }

    mReadyBuffer.insert(mReadyBuffer.end(), data, data + size);
    if (needsNewline)
    {
        mReadyBuffer.push_back('\n');
    }
}

} // namespace loglib::internal

namespace loglib
{

UdpServerProducer::UdpServerProducer(Options options)
    : mImpl(std::make_unique<internal::UdpServerProducerImpl>(std::move(options)))
{
}

UdpServerProducer::~UdpServerProducer() = default;

size_t UdpServerProducer::Read(std::span<char> buffer)
{
    return mImpl->Read(buffer);
}

void UdpServerProducer::WaitForBytes(std::chrono::milliseconds timeout)
{
    mImpl->WaitForBytes(timeout);
}

void UdpServerProducer::Stop() noexcept
{
    mImpl->Stop();
}

bool UdpServerProducer::IsClosed() const noexcept
{
    return mImpl->IsClosed();
}

std::string UdpServerProducer::DisplayName() const
{
    return mImpl->DisplayName();
}

void UdpServerProducer::SetStatusCallback(std::function<void(SourceStatus)> callback)
{
    mImpl->SetStatusCallback(std::move(callback));
}

uint16_t UdpServerProducer::BoundPort() const noexcept
{
    return mImpl->BoundPort();
}

size_t UdpServerProducer::DatagramCount() const noexcept
{
    return mImpl->DatagramCount();
}

size_t UdpServerProducer::DroppedByteCount() const noexcept
{
    return mImpl->DroppedByteCount();
}

} // namespace loglib
