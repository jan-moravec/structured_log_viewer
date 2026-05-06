#include "loglib/udp_server_producer.hpp"

#include "loglib/internal/line_bytes_queue.hpp"

#include <asio.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
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

    /// Edge-triggered transition `Waiting -> Running` once a real
    /// datagram has arrived. Mirrors `TcpServerProducer::MarkRunning`
    /// so the GUI badge has the same semantics across protocols
    /// (bound-but-no-traffic is `Waiting`, first byte flips to
    /// `Running`, and the producer never falls back).
    void MarkRunning();

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

    /// Byte queue (FIFO). Backed by a `std::deque<char>` ring so
    /// back-pressure-induced front drops are O(K-bytes-dropped) and
    /// never trigger a full-buffer shift. The wrapper handles the
    /// line-aware drop policy so this file stays focused on Asio
    /// plumbing.
    LineBytesQueue mReadyBuffer;

    std::atomic<bool> mStopRequested{false};
    std::atomic<bool> mWorkerExited{false};
    std::atomic<size_t> mDatagramCount{0};
    std::atomic<size_t> mDroppedByteCount{0};

    uint16_t mBoundPort = 0;

    /// Status callback bookkeeping. Edge-triggered. We start in
    /// `Waiting` (socket bound but no datagrams seen yet) and flip to
    /// `Running` exactly once on the first arrival, parity with
    /// `TcpServerProducer`. UDP has no per-connection state to track
    /// past that, so the status never falls back to `Waiting`.
    mutable std::mutex mCallbackMutex;
    std::function<void(SourceStatus)> mStatusCallback;
    SourceStatus mLastReportedStatus = SourceStatus::Waiting;

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
    const std::scoped_lock lock(mMutex);
    return mReadyBuffer.Read(buffer);
}

void UdpServerProducerImpl::WaitForBytes(std::chrono::milliseconds timeout)
{
    if (timeout.count() <= 0)
    {
        return; // non-blocking
    }
    std::unique_lock<std::mutex> lock(mMutex);
    mCv.wait_for(lock, timeout, [&] {
        return !mReadyBuffer.Empty() || mStopRequested.load(std::memory_order_acquire) ||
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
    // the running `async_receive_from` and the destructor. The close
    // cancels the in-flight receive with `operation_aborted`; the
    // completion handler observes `mStopRequested` and does not
    // re-arm, so the io_context's work count drops to zero and
    // `run()` returns naturally. We deliberately do NOT call
    // `mIoContext.stop()` -- that would abandon the posted close
    // handler and leave the close-on-destruct fallback to clean up.
    try
    {
        asio::post(mIoContext, [this] {
            asio::error_code ec;
            mSocket.close(ec);
        });
    }
    catch (...)
    {
        // `asio::post` can theoretically throw on allocator failure;
        // hard-stop as the safety net so the producer always terminates.
        mIoContext.stop();
    }
    mCv.notify_all();
}

bool UdpServerProducerImpl::IsClosed() const noexcept
{
    if (!mWorkerExited.load(std::memory_order_acquire))
    {
        return false;
    }
    const std::scoped_lock lock(mMutex);
    return mReadyBuffer.Empty();
}

std::string UdpServerProducerImpl::DisplayName() const
{
    return mDisplayName;
}

void UdpServerProducerImpl::SetStatusCallback(std::function<void(SourceStatus)> callback)
{
    std::function<void(SourceStatus)> snapshot;
    SourceStatus current{};
    {
        const std::scoped_lock lock(mCallbackMutex);
        mStatusCallback = std::move(callback);
        snapshot = mStatusCallback;
        current = mLastReportedStatus;
    }
    // Fire once with the current state so the GUI gets a fresh
    // edge-triggered baseline. UDP starts in `Waiting`; `MarkRunning`
    // promotes to `Running` on the first datagram. Replaying the
    // current state matches the TCP producer's behaviour so consumers
    // installed late still see the live status.
    if (snapshot)
    {
        snapshot(current);
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

void UdpServerProducerImpl::MarkRunning()
{
    std::function<void(SourceStatus)> cb;
    {
        const std::scoped_lock lock(mCallbackMutex);
        if (mLastReportedStatus == SourceStatus::Running)
        {
            return;
        }
        mLastReportedStatus = SourceStatus::Running;
        cb = mStatusCallback;
    }
    if (cb)
    {
        cb(SourceStatus::Running);
    }
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
                    const std::scoped_lock lock(mMutex);
                    AppendDatagramLocked(mRecvBuffer.data(), bytes);
                }
                mDatagramCount.fetch_add(1, std::memory_order_acq_rel);
                MarkRunning();
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
    // Normalise the trailing newline first so the line-aware
    // back-pressure path inside `LineBytesQueue::Append` always sees
    // a complete-line payload (the framing contract the parser
    // expects). The synthetic newline goes through a stack buffer
    // when the datagram is large enough to need it; for the common
    // pre-terminated case we hand the original pointer through.
    if (data[size - 1] == '\n')
    {
        mReadyBuffer.Append(data, size, mOptions.maxQueueBytes, mDroppedByteCount);
    }
    else
    {
        // One pass: append the datagram and the synthetic newline as
        // separate ops -- both share the same back-pressure cap, so
        // the second call will drop additional history if needed
        // without ever creating a torn line.
        mReadyBuffer.Append(data, size, mOptions.maxQueueBytes, mDroppedByteCount);
        const char nl = '\n';
        mReadyBuffer.Append(&nl, 1, mOptions.maxQueueBytes, mDroppedByteCount);
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
