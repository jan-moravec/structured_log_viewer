#pragma once

#include "bytes_producer.hpp"

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

/// `BytesProducer` over a TCP listening socket. Accepts multiple
/// concurrent client connections, drains each via `async_read_some`,
/// and interleaves their lines into the shared byte queue at line
/// granularity (per-session carry buffer; only complete lines are
/// flushed into the shared queue, so concurrent senders never
/// produce torn records). Optional TLS via `asio::ssl` when the
/// project is built with `LOGLIB_NETWORK_TLS=ON`.
///
/// Design summary:
///   - One `asio::io_context` worker thread per producer drives both
///     accept and per-session reads. Sessions are reference-counted
///     `shared_ptr`s so the in-flight async chain keeps them alive
///     until the read completes.
///   - Per-session carry buffer guarantees the shared queue is
///     append-only at line boundaries: a partial line from peer A
///     never interleaves into a partial line from peer B. On a peer
///     disconnect the carry is flushed as a synthetic final line.
///   - Source addresses are dropped: the GUI shows one interleaved
///     view across all peers.
///   - `SetStatusCallback` reports `Waiting` until the first byte
///     arrives from any client, then `Running`. It does not flip back
///     to `Waiting` on an idle period: TCP listening with no clients
///     is normal, not an environmental fault.
class TcpServerProducer final : public BytesProducer
{
public:
    /// TLS configuration. Pass `Options::tls = std::nullopt` for
    /// plaintext. When the project is built with
    /// `LOGLIB_NETWORK_TLS=OFF`, supplying any TLS options causes
    /// the constructor to throw `std::runtime_error`.
    struct TlsOptions
    {
        /// PEM file with the server's certificate chain (leaf first).
        std::filesystem::path certificateChain;

        /// PEM file with the matching private key (RSA or ECDSA).
        /// Encrypted keys are not supported by the current
        /// implementation -- decrypt offline before pointing at it.
        std::filesystem::path privateKey;

        /// Optional PEM CA bundle used to verify client certificates.
        /// Empty path means client certs are not validated.
        std::filesystem::path caBundle;

        /// When true and `caBundle` is set, the handshake fails if
        /// the client does not present a certificate the bundle
        /// trusts. When false, client certs are optional even if
        /// `caBundle` is set.
        bool requireClientCertificate = false;
    };

    struct Options
    {
        /// Bind address. `"0.0.0.0"` for IPv4-any, `"::"` for IPv6
        /// dual-stack, or a specific interface IP.
        std::string bindAddress = "0.0.0.0";

        /// TCP port to listen on. `0` requests an OS-assigned port,
        /// useful for tests.
        uint16_t port = 0;

        /// Hard cap on simultaneous accepted connections. New
        /// connections beyond this are accepted-and-immediately-closed
        /// so the OS-side accept queue does not back up indefinitely;
        /// the rejection is counted in `TotalClientsRejected()`.
        size_t maxConcurrentClients = 16;

        /// `async_read_some` chunk size per session. 64 KiB amortises
        /// syscall cost while keeping per-session memory bounded.
        size_t readChunkBytes = 64 * 1024;

        /// Soft cap on the byte queue. When the parser falls behind
        /// and the queue exceeds this, the oldest queued bytes are
        /// dropped (rounded up to the next newline) and counted in
        /// `DroppedByteCount`. `0` disables back-pressure.
        size_t maxQueueBytes = 16 * 1024 * 1024;

        /// `std::nullopt` for plaintext. Filled in to enable TLS.
        /// `LOGLIB_NETWORK_TLS=OFF` builds reject any non-nullopt
        /// value at the constructor.
        std::optional<TlsOptions> tls;
    };

    /// Construct, bind the listening socket, optionally initialise
    /// the TLS context, and spawn the I/O worker thread. Throws
    /// `std::system_error` on bind / listen failure or
    /// `std::runtime_error` for TLS misconfiguration (missing files,
    /// build without TLS, etc.).
    explicit TcpServerProducer(Options options);

    ~TcpServerProducer() override;

    TcpServerProducer(const TcpServerProducer &) = delete;
    TcpServerProducer &operator=(const TcpServerProducer &) = delete;
    TcpServerProducer(TcpServerProducer &&) = delete;
    TcpServerProducer &operator=(TcpServerProducer &&) = delete;

    size_t Read(std::span<char> buffer) override;

    void WaitForBytes(std::chrono::milliseconds timeout) override;

    /// Stop accepting connections, close every active session, drain
    /// the I/O context, and signal the worker to exit. Idempotent.
    void Stop() noexcept override;

    [[nodiscard]] bool IsClosed() const noexcept override;

    /// e.g. `"tcp://0.0.0.0:5140"` (or `"tcp+tls://..."` when TLS is
    /// configured). Resolved to the actually-bound port when
    /// `Options::port == 0`.
    [[nodiscard]] std::string DisplayName() const override;

    void SetStatusCallback(const std::function<void(SourceStatus)> &callback) override;

    /// Actual port the listener is bound to.
    [[nodiscard]] uint16_t BoundPort() const noexcept;

    /// Currently-active session count. Drops as clients disconnect.
    [[nodiscard]] size_t ActiveClientCount() const noexcept;

    /// Cumulative count of accepted-and-served sessions since
    /// construction. Excludes connections rejected by the cap.
    [[nodiscard]] size_t TotalClientsAccepted() const noexcept;

    /// Cumulative count of connections rejected because
    /// `Options::maxConcurrentClients` was reached. Useful for tests.
    [[nodiscard]] size_t TotalClientsRejected() const noexcept;

    /// Cumulative bytes dropped from the queue under back-pressure.
    [[nodiscard]] size_t DroppedByteCount() const noexcept;

private:
    std::unique_ptr<internal::TcpServerProducerImpl> mImpl;
};

} // namespace loglib
