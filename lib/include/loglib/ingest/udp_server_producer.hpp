/**
 * @brief Byte producer that listens on a UDP port.
 *
 * A UdpServerProducer receives datagrams and ends each one with a newline if needed, so every datagram becomes one or
 * more complete lines for the parser.
 *
 * This is part of the ingest module.
 */
#pragma once

#include "loglib/core/bytes_producer.hpp"

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

/**
 * @brief A BytesProducer that listens on a UDP port and turns each datagram into complete lines.
 *
 * A datagram that does not end with a newline gets one, so datagram boundaries always match line boundaries for the
 * parser. Every datagram must hold one or more complete records, because records are not joined across datagrams and
 * lost packets would corrupt such a stream anyway.
 *
 * - One I/O thread receives datagrams in a loop.
 * - The sender address is not kept, so the result is one merged view of all senders.
 * - When the parser falls behind and the queue passes maxQueueBytes, the oldest bytes are dropped and counted in
 *   DroppedByteCount.
 */
class UdpServerProducer final : public BytesProducer
{
public:
    // NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
    struct Options
    {
        /**
         * @brief The address to listen on: "0.0.0.0" for any IPv4 address, "::" for IPv6 and IPv4, or the address of
         * one interface. An address that cannot be parsed makes the constructor throw std::system_error.
         */
        std::string bindAddress = "0.0.0.0";

        /**
         * @brief The port to listen on. Zero lets the operating system choose, which suits tests. Other callers should
         * choose a fixed port.
         */
        uint16_t port = 0;

        /**
         * @brief The receive buffer size, which is also the largest datagram accepted. A longer datagram is cut off by
         * the operating system. 64 KiB is the IPv4 maximum.
         */
        size_t maxDatagramBytes = 64 * 1024;

        /**
         * @brief The soft limit for the byte queue. When the parser falls behind and the queue passes it, the oldest
         * bytes are dropped and counted in DroppedByteCount. Zero turns the limit off, and then the queue can grow
         * without end.
         */
        size_t maxQueueBytes = 16 * 1024 * 1024;
    };
    // NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

    /**
     * @brief Binds the socket and starts the I/O thread. Throws std::system_error if the address cannot be parsed or
     * the socket cannot be bound.
     */
    explicit UdpServerProducer(Options options);

    ~UdpServerProducer() override;

    UdpServerProducer(const UdpServerProducer &) = delete;
    UdpServerProducer &operator=(const UdpServerProducer &) = delete;
    UdpServerProducer(UdpServerProducer &&) = delete;
    UdpServerProducer &operator=(UdpServerProducer &&) = delete;

    size_t Read(std::span<char> buffer) override;

    void WaitForBytes(std::chrono::milliseconds timeout) override;

    /**
     * @brief Stops receiving, wakes any blocked Read or WaitForBytes and ends the I/O thread. Safe from any thread and
     * safe to call twice.
     */
    void Stop() noexcept override;

    [[nodiscard]] bool IsClosed() const noexcept override;

    /**
     * @brief The address being served, such as udp://0.0.0.0:5141. The port is the one actually bound when
     * Options::port is zero.
     */
    [[nodiscard]] std::string DisplayName() const override;

    void SetStatusCallback(const std::function<void(SourceStatus)> &callback) override;

    /** @brief The port the socket is bound to. It is Options::port, or the port the system chose if that was zero. */
    [[nodiscard]] uint16_t BoundPort() const noexcept;

    /** @brief How many datagrams were received. It includes datagrams whose bytes were later dropped from the queue. */
    [[nodiscard]] size_t DatagramCount() const noexcept;

    /**
     * @brief How many bytes were dropped from the queue because it was full. Zero when the limit is off or the parser
     * kept up.
     */
    [[nodiscard]] size_t DroppedByteCount() const noexcept;

private:
    std::unique_ptr<internal::UdpServerProducerImpl> mImpl;
};

} // namespace loglib
