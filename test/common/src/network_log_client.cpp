#include "test_common/network_log_client.hpp"

#include <asio.hpp>
#ifdef LOGLIB_HAS_TLS
#include <asio/ssl.hpp>
#endif

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace test_common::internal
{

class TcpLogClientImpl
{
public:
    TcpLogClientImpl(std::string host, uint16_t port, std::optional<TcpLogClient::TlsOptions> tls);
    ~TcpLogClientImpl();

    TcpLogClientImpl(const TcpLogClientImpl &) = delete;
    TcpLogClientImpl &operator=(const TcpLogClientImpl &) = delete;
    TcpLogClientImpl(TcpLogClientImpl &&) = delete;
    TcpLogClientImpl &operator=(TcpLogClientImpl &&) = delete;

    void Send(std::string_view line);
    void SendRaw(std::string_view bytes);
    void Close();

private:
    /// Single point of byte-level write so plain and TLS share the
    /// error-handling path. Used by both `Send` (with newline-fixup)
    /// and `SendRaw` (verbatim).
    void WriteAll(const char *data, size_t size);

    asio::io_context mIoContext;
    asio::ip::tcp::socket mSocket;
#ifdef LOGLIB_HAS_TLS
    std::optional<asio::ssl::context> mSslContext;
    std::optional<asio::ssl::stream<asio::ip::tcp::socket &>> mSslStream;
#endif
    bool mUsesTls = false;
};

TcpLogClientImpl::TcpLogClientImpl(std::string host, uint16_t port, std::optional<TcpLogClient::TlsOptions> tls)
    : mSocket(mIoContext)
{
    asio::error_code ec;
    asio::ip::tcp::resolver resolver(mIoContext);
    auto endpoints = resolver.resolve(host, std::to_string(port), ec);
    if (ec)
    {
        throw std::runtime_error(
            "TcpLogClient: failed to resolve " + host + ":" + std::to_string(port) + ": " + ec.message()
        );
    }

    asio::connect(mSocket, endpoints, ec);
    if (ec)
    {
        throw std::runtime_error(
            "TcpLogClient: failed to connect to " + host + ":" + std::to_string(port) + ": " + ec.message()
        );
    }

    if (tls.has_value())
    {
#ifdef LOGLIB_HAS_TLS
        mUsesTls = true;
        mSslContext.emplace(asio::ssl::context::tls_client);
        mSslContext->set_options(
            asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2 |
            asio::ssl::context::no_sslv3 | asio::ssl::context::no_tlsv1 | asio::ssl::context::no_tlsv1_1
        );

        if (!tls->caBundle.empty())
        {
            mSslContext->load_verify_file(tls->caBundle.string(), ec);
            if (ec)
            {
                throw std::runtime_error(
                    "TcpLogClient: failed to load CA bundle '" + tls->caBundle.string() + "': " + ec.message()
                );
            }
        }
        if (!tls->clientCert.empty() && !tls->clientKey.empty())
        {
            mSslContext->use_certificate_chain_file(tls->clientCert.string(), ec);
            if (ec)
            {
                throw std::runtime_error(
                    "TcpLogClient: failed to load client cert '" + tls->clientCert.string() + "': " + ec.message()
                );
            }
            mSslContext->use_private_key_file(tls->clientKey.string(), asio::ssl::context::pem, ec);
            if (ec)
            {
                throw std::runtime_error(
                    "TcpLogClient: failed to load client key '" + tls->clientKey.string() + "': " + ec.message()
                );
            }
        }

        mSslStream.emplace(mSocket, *mSslContext);
        if (tls->insecureSkipVerify)
        {
            mSslStream->set_verify_mode(asio::ssl::verify_none);
        }
        else
        {
            mSslStream->set_verify_mode(asio::ssl::verify_peer);
        }

        const std::string sni = tls->serverNameIndication.empty() ? host : tls->serverNameIndication;
        if (!sni.empty())
        {
            // SNI extension; safe to ignore failure (older servers).
            // OpenSSL exposes this as SSL_set_tlsext_host_name on the
            // native handle.
            SSL_set_tlsext_host_name(mSslStream->native_handle(), sni.c_str());
        }

        mSslStream->handshake(asio::ssl::stream_base::client, ec);
        if (ec)
        {
            throw std::runtime_error(
                "TcpLogClient: TLS handshake to " + host + ":" + std::to_string(port) + " failed: " + ec.message()
            );
        }
#else
        throw std::runtime_error(
            "TcpLogClient: TLS requested but the binary was built without LOGLIB_HAS_TLS"
        );
#endif
    }
}

TcpLogClientImpl::~TcpLogClientImpl()
{
    asio::error_code ec;
#ifdef LOGLIB_HAS_TLS
    if (mUsesTls && mSslStream.has_value())
    {
        mSslStream->shutdown(ec);
    }
#endif
    mSocket.close(ec);
}

void TcpLogClientImpl::Send(std::string_view line)
{
    const bool needsNewline = line.empty() || line.back() != '\n';
    WriteAll(line.data(), line.size());
    if (needsNewline)
    {
        const char nl = '\n';
        WriteAll(&nl, 1);
    }
}

void TcpLogClientImpl::SendRaw(std::string_view bytes)
{
    if (!bytes.empty())
    {
        WriteAll(bytes.data(), bytes.size());
    }
}

void TcpLogClientImpl::WriteAll(const char *data, size_t size)
{
    asio::error_code ec;
    if (mUsesTls)
    {
#ifdef LOGLIB_HAS_TLS
        asio::write(*mSslStream, asio::buffer(data, size), ec);
#endif
    }
    else
    {
        asio::write(mSocket, asio::buffer(data, size), ec);
    }
    if (ec)
    {
        throw std::runtime_error("TcpLogClient: write failed: " + ec.message());
    }
}

void TcpLogClientImpl::Close()
{
    asio::error_code ec;
#ifdef LOGLIB_HAS_TLS
    if (mUsesTls && mSslStream.has_value())
    {
        mSslStream->shutdown(ec);
        mSslStream.reset();
    }
#endif
    mSocket.close(ec);
}

class UdpLogClientImpl
{
public:
    UdpLogClientImpl(std::string host, uint16_t port);
    ~UdpLogClientImpl();

    UdpLogClientImpl(const UdpLogClientImpl &) = delete;
    UdpLogClientImpl &operator=(const UdpLogClientImpl &) = delete;
    UdpLogClientImpl(UdpLogClientImpl &&) = delete;
    UdpLogClientImpl &operator=(UdpLogClientImpl &&) = delete;

    size_t Send(std::string_view line);
    void Close();

private:
    asio::io_context mIoContext;
    asio::ip::udp::socket mSocket;
    asio::ip::udp::endpoint mPeer;
};

UdpLogClientImpl::UdpLogClientImpl(std::string host, uint16_t port) : mSocket(mIoContext)
{
    asio::error_code ec;
    asio::ip::udp::resolver resolver(mIoContext);
    auto endpoints = resolver.resolve(host, std::to_string(port), ec);
    if (ec || endpoints.empty())
    {
        throw std::runtime_error(
            "UdpLogClient: failed to resolve " + host + ":" + std::to_string(port) +
            (ec ? ": " + ec.message() : ": no endpoints")
        );
    }
    mPeer = *endpoints.begin();
    mSocket.open(mPeer.protocol(), ec);
    if (ec)
    {
        throw std::runtime_error("UdpLogClient: socket open failed: " + ec.message());
    }
}

UdpLogClientImpl::~UdpLogClientImpl()
{
    asio::error_code ec;
    mSocket.close(ec);
}

size_t UdpLogClientImpl::Send(std::string_view line)
{
    const bool needsNewline = line.empty() || line.back() != '\n';

    // Build a single datagram so the network sees one packet per
    // call. Two `send_to`s would split the record across packets.
    std::vector<char> buf;
    buf.reserve(line.size() + (needsNewline ? 1 : 0));
    buf.insert(buf.end(), line.begin(), line.end());
    if (needsNewline)
    {
        buf.push_back('\n');
    }

    asio::error_code ec;
    const size_t sent = mSocket.send_to(asio::buffer(buf.data(), buf.size()), mPeer, 0, ec);
    if (ec)
    {
        // Windows reports `connection_refused` on UDP if a previous
        // datagram triggered an ICMP unreachable; the kernel caches
        // it and surfaces it to the *next* send. Swallow that
        // specific transient -- we have no peer state to clean up.
        if (ec == asio::error::connection_refused)
        {
            return 0;
        }
        throw std::runtime_error("UdpLogClient: send failed: " + ec.message());
    }
    return sent;
}

void UdpLogClientImpl::Close()
{
    asio::error_code ec;
    mSocket.close(ec);
}

} // namespace test_common::internal

namespace test_common
{

TcpLogClient::TcpLogClient(std::string host, uint16_t port, std::optional<TlsOptions> tls)
    : mImpl(std::make_unique<internal::TcpLogClientImpl>(std::move(host), port, std::move(tls)))
{
}

TcpLogClient::~TcpLogClient() = default;

TcpLogClient::TcpLogClient(TcpLogClient &&) noexcept = default;
TcpLogClient &TcpLogClient::operator=(TcpLogClient &&) noexcept = default;

void TcpLogClient::Send(std::string_view line)
{
    mImpl->Send(line);
}

void TcpLogClient::SendRaw(std::string_view bytes)
{
    mImpl->SendRaw(bytes);
}

void TcpLogClient::Close()
{
    mImpl->Close();
}

UdpLogClient::UdpLogClient(std::string host, uint16_t port)
    : mImpl(std::make_unique<internal::UdpLogClientImpl>(std::move(host), port))
{
}

UdpLogClient::~UdpLogClient() = default;

UdpLogClient::UdpLogClient(UdpLogClient &&) noexcept = default;
UdpLogClient &UdpLogClient::operator=(UdpLogClient &&) noexcept = default;

size_t UdpLogClient::Send(std::string_view line)
{
    return mImpl->Send(line);
}

void UdpLogClient::Close()
{
    mImpl->Close();
}

} // namespace test_common
