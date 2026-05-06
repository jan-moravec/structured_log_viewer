#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace test_common
{

namespace internal
{
class TcpLogClientImpl;
class UdpLogClientImpl;
} // namespace internal

/// Synchronous TCP client used by both the network-producer unit
/// tests and the `log_generator` manual driver. Connects on
/// construction and sends one log line per `Send` call.
///
/// The implementation owns its own `asio::io_context` (run on the
/// caller's thread for `Send`) so the helper is single-threaded and
/// blocking, which is what tests want.
class TcpLogClient
{
public:
    struct TlsOptions
    {
        /// Optional CA bundle used to verify the server certificate.
        /// Empty path means the system trust store is used; a self-
        /// signed dev cert needs a dedicated bundle here (or
        /// `insecureSkipVerify`).
        std::filesystem::path caBundle;

        /// Skip server certificate validation entirely. Tests with
        /// self-signed certs use this; production code never should.
        bool insecureSkipVerify = false;

        /// Client cert + key for mutual TLS. Both empty disables
        /// client-cert auth.
        std::filesystem::path clientCert;
        std::filesystem::path clientKey;

        /// SNI hostname to send during the handshake. Defaults to
        /// the connection host when empty.
        std::string serverNameIndication;
    };

    /// Connect (and TLS-handshake when @p tls is non-nullopt). Throws
    /// `std::runtime_error` on connect / handshake failure.
    /// `tls` non-nullopt in a build without `LOGLIB_HAS_TLS` throws.
    TcpLogClient(std::string host, uint16_t port, std::optional<TlsOptions> tls = std::nullopt);

    ~TcpLogClient();

    TcpLogClient(const TcpLogClient &) = delete;
    TcpLogClient &operator=(const TcpLogClient &) = delete;
    TcpLogClient(TcpLogClient &&) noexcept;
    TcpLogClient &operator=(TcpLogClient &&) noexcept;

    /// Send @p line. A trailing `\n` is appended if the line does not
    /// already end with one. Throws on write failure.
    void Send(std::string_view line);

    /// Send @p bytes verbatim. No newline is appended. Used by tests
    /// that want to demonstrate line-fragment interleave and by
    /// adversarial drivers that craft byte patterns the framer must
    /// tolerate.
    void SendRaw(std::string_view bytes);

    /// Cleanly shut down the connection. Idempotent; the destructor
    /// also calls this.
    void Close();

private:
    std::unique_ptr<internal::TcpLogClientImpl> mImpl;
};

/// Synchronous UDP client. Each `Send` emits exactly one datagram
/// to the configured peer; `\n` is appended if missing so the
/// matching `UdpServerProducer` framing is preserved.
class UdpLogClient
{
public:
    /// Resolve @p host and create an unconnected UDP socket. Throws
    /// `std::runtime_error` if @p host cannot be resolved.
    UdpLogClient(std::string host, uint16_t port);

    ~UdpLogClient();

    UdpLogClient(const UdpLogClient &) = delete;
    UdpLogClient &operator=(const UdpLogClient &) = delete;
    UdpLogClient(UdpLogClient &&) noexcept;
    UdpLogClient &operator=(UdpLogClient &&) noexcept;

    /// Send one datagram. Returns the number of bytes sent (= line
    /// length, possibly + 1 for the appended `\n`). Throws on
    /// permanent send failure; transient ICMP-derived errors on
    /// Windows are swallowed.
    size_t Send(std::string_view line);

    /// Closes the socket. Idempotent.
    void Close();

private:
    std::unique_ptr<internal::UdpLogClientImpl> mImpl;
};

} // namespace test_common
