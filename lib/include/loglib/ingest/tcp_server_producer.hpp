/**
 * @brief Byte producer that listens on a TCP port.
 *
 * A TcpServerProducer accepts several clients at once and merges their lines into one stream, one complete line at a
 * time, so concurrent senders never interleave inside a line. TLS is available when the library is built with it.
 *
 * This is part of the ingest module.
 */
#pragma once

#include "loglib/core/bytes_producer.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace loglib
{

namespace internal
{
class TcpServerProducerImpl; // pimpl forward decl
}

/**
 * @brief A BytesProducer that listens on a TCP port and merges the lines of all clients into one stream.
 *
 * It accepts several clients at once and reads each of them. Only complete lines go into the shared queue, so lines
 * from different senders never mix inside a line. TLS is available when the library is built with LOGLIB_NETWORK_TLS.
 *
 * - One I/O thread runs both the accepting and the reading of all clients. A client lives until its read chain ends.
 * - Each client has its own buffer for the partial line it is sending. When the client disconnects, that partial line
 *   is delivered as a last line.
 * - The sender addresses are not kept, so the result is one merged view of all senders.
 * - The status callback reports Waiting until the first byte arrives from any client, and Running after that. It does
 *   not go back to Waiting when things are quiet, because a listener with no clients is normal.
 */
class TcpServerProducer final : public BytesProducer
{
public:
    /**
     * @brief Settings for encrypted connections. Leave Options::tls empty for plain TCP.
     *
     * In a build without LOGLIB_NETWORK_TLS, giving any TLS settings makes the constructor throw std::runtime_error.
     */
    struct TlsOptions
    {
        /** @brief PEM file with the server's certificate chain, the server certificate first. */
        std::filesystem::path certificateChain;

        /**
         * @brief PEM file with the matching private key, RSA or ECDSA. Encrypted keys are not supported, so decrypt
         * the key first.
         */
        std::filesystem::path privateKey;

        /**
         * @brief PEM file with the certificate authorities used to check client certificates. An empty path means
         * client certificates are not checked.
         */
        std::filesystem::path caBundle;

        /**
         * @brief Makes the handshake fail unless the client presents a certificate that caBundle trusts.
         *
         * It only has an effect when caBundle is set. Otherwise client certificates are optional.
         */
        bool requireClientCertificate = false;
    };

    // NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
    struct Options
    {
        /**
         * @brief The address to listen on: "0.0.0.0" for any IPv4 address, "::" for IPv6 and IPv4, or the address of
         * one interface.
         */
        std::string bindAddress = "0.0.0.0";

        /** @brief The port to listen on. Zero lets the operating system choose, which tests use. */
        uint16_t port = 0;

        /**
         * @brief The most clients served at the same time.
         *
         * A connection beyond the limit is accepted and closed at once, so that the system's accept queue does not grow
         * without end. It is counted in TotalClientsRejected.
         */
        size_t maxConcurrentClients = 16;

        /**
         * @brief How many bytes one read takes for each client. 64 KiB keeps the per-client memory small without many
         * calls.
         */
        size_t readChunkBytes = 64 * 1024;

        /**
         * @brief The soft limit for the byte queue.
         *
         * When the parser falls behind and the queue grows past it, the oldest bytes are dropped up to the next newline
         * and counted in DroppedByteCount. Zero turns the limit off.
         */
        size_t maxQueueBytes = 16 * 1024 * 1024;

        /**
         * @brief The longest line held for each client while waiting for its newline. A longer line is dropped up to
         * its newline and counted in DroppedByteCount.
         */
        size_t maxLineBytes = DEFAULT_MAX_LINE_BYTES;

        /** @brief TLS settings. Leave empty for plain TCP. A build without LOGLIB_NETWORK_TLS throws if this is set. */
        std::optional<TlsOptions> tls;
    };
    // NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

    /**
     * @brief Binds the port, sets up TLS if requested, and starts the I/O thread.
     *
     * Throws std::system_error if binding or listening fails, and std::runtime_error for TLS problems such as missing
     * files or a build without TLS.
     */
    explicit TcpServerProducer(Options options);

    ~TcpServerProducer() override;

    TcpServerProducer(const TcpServerProducer &) = delete;
    TcpServerProducer &operator=(const TcpServerProducer &) = delete;
    TcpServerProducer(TcpServerProducer &&) = delete;
    TcpServerProducer &operator=(TcpServerProducer &&) = delete;

    size_t Read(std::span<char> buffer) override;

    void WaitForBytes(std::chrono::milliseconds timeout) override;

    /**
     * @brief Stops accepting, closes every client, lets pending I/O finish and ends the I/O thread. Safe to call
     * twice.
     */
    void Stop() noexcept override;

    [[nodiscard]] bool IsClosed() const noexcept override;

    /**
     * @brief The address being served, such as tcp://0.0.0.0:5140, or tcp+tls:// with TLS. The port is the one
     * actually bound when Options::port is zero.
     */
    [[nodiscard]] std::string DisplayName() const override;

    void SetStatusCallback(const std::function<void(SourceStatus)> &callback) override;

    /** @brief The port the listener is bound to. */
    [[nodiscard]] uint16_t BoundPort() const noexcept;

    /** @brief How many clients are connected now. */
    [[nodiscard]] size_t ActiveClientCount() const noexcept;

    /** @brief How many connections were accepted and served so far. Rejected ones are not counted. */
    [[nodiscard]] size_t TotalClientsAccepted() const noexcept;

    /** @brief How many connections were turned away because Options::maxConcurrentClients was reached. */
    [[nodiscard]] size_t TotalClientsRejected() const noexcept;

    /** @brief How many bytes were dropped from the queue because it was full. */
    [[nodiscard]] size_t DroppedByteCount() const noexcept;

private:
    std::unique_ptr<internal::TcpServerProducerImpl> mImpl;
};

} // namespace loglib
