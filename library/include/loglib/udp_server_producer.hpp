#pragma once

#include "bytes_producer.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>

namespace loglib
{

namespace internal
{
class UdpServerProducerImpl; // pimpl forward decl
}

/// `BytesProducer` over a UDP socket. Receives datagrams on the
/// configured bind endpoint, normalises trailing newlines (so each
/// datagram lands as one or more complete log lines in the parser's
/// view), and queues the bytes for the streaming pipeline to drain.
///
/// Design summary:
///   - One `asio::io_context` worker thread per producer running an
///     `async_receive_from` reschedule loop.
///   - Datagrams are NOT framed across packets: each datagram must
///     contain one or more complete records. Records straddling
///     datagram boundaries are not supported (UDP packet loss would
///     silently corrupt the line stream anyway).
///   - The producer appends a synthetic `\n` to any datagram missing
///     one so datagram boundaries always match line boundaries
///     downstream of `RunStreamingParseLoop`.
///   - Source address is dropped: the GUI displays one interleaved
///     view across all peers.
///   - `maxQueueBytes` provides simple back-pressure: when the parser
///     falls behind, the oldest queued bytes are dropped and counted
///     in `DroppedByteCount`.
class UdpServerProducer final : public BytesProducer
{
public:
    struct Options
    {
        /// Bind address. `"0.0.0.0"` for IPv4-any, `"::"` for IPv6-any
        /// dual-stack, or a specific interface IP. Resolved via Asio's
        /// `from_string`; the ctor throws `std::system_error` on parse
        /// or bind failure.
        std::string bindAddress = "0.0.0.0";

        /// UDP port to listen on. `0` requests an OS-assigned port,
        /// useful for tests; production callers should pin a port.
        uint16_t port = 0;

        /// Receive-buffer size, also the maximum size of a single
        /// datagram accepted. Datagrams larger than this are silently
        /// truncated by the OS at recv time. 64 KiB matches the IPv4
        /// upper bound for datagram size.
        size_t maxDatagramBytes = 64 * 1024;

        /// Soft cap on the byte queue. When the parser falls behind
        /// and the queue exceeds this, the oldest queued bytes are
        /// dropped (counted in `DroppedByteCount`). `0` disables
        /// back-pressure (the queue may grow unbounded).
        size_t maxQueueBytes = 16 * 1024 * 1024;
    };

    /// Construct, bind the socket, and spawn the I/O worker thread.
    /// Throws `std::system_error` if `Options::bindAddress` cannot be
    /// parsed or the socket cannot be bound.
    explicit UdpServerProducer(Options options);

    ~UdpServerProducer() override;

    UdpServerProducer(const UdpServerProducer &) = delete;
    UdpServerProducer &operator=(const UdpServerProducer &) = delete;
    UdpServerProducer(UdpServerProducer &&) = delete;
    UdpServerProducer &operator=(UdpServerProducer &&) = delete;

    size_t Read(std::span<char> buffer) override;

    void WaitForBytes(std::chrono::milliseconds timeout) override;

    /// Stop accepting datagrams, unblock any in-flight `Read` /
    /// `WaitForBytes`, and signal the worker to exit. Idempotent.
    /// Safe from any thread.
    void Stop() noexcept override;

    [[nodiscard]] bool IsClosed() const noexcept override;

    /// e.g. `"udp://0.0.0.0:5141"`. Resolves to the actually-bound
    /// port when `Options::port == 0`.
    [[nodiscard]] std::string DisplayName() const override;

    void SetStatusCallback(const std::function<void(SourceStatus)> &callback) override;

    /// Actual port the socket is bound to. Equals `Options::port`
    /// unless that was 0, in which case it returns the kernel-chosen
    /// port. Stable for the producer's lifetime once the ctor returns.
    [[nodiscard]] uint16_t BoundPort() const noexcept;

    /// Cumulative datagrams received since construction. Includes any
    /// dropped under back-pressure (drops happen at the queue, not at
    /// the recv layer).
    [[nodiscard]] size_t DatagramCount() const noexcept;

    /// Cumulative bytes dropped from the queue under back-pressure.
    /// `0` when `Options::maxQueueBytes == 0` or when the parser kept
    /// up the whole session.
    [[nodiscard]] size_t DroppedByteCount() const noexcept;

private:
    std::unique_ptr<internal::UdpServerProducerImpl> mImpl;
};

} // namespace loglib
