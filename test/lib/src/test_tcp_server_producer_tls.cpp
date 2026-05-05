#include <loglib/tcp_server_producer.hpp>

#include <loglib_test/scaled_ms.hpp>
#include <test_common/network_log_client.hpp>

#include <catch2/catch_all.hpp>

#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <random>
#include <span>
#include <stdexcept>
#include <string>

using loglib::TcpServerProducer;
using loglib_test::ScaledMs;
using namespace std::chrono_literals;

namespace
{

/// RAII scratch dir for the TLS test fixtures (cert + key PEM).
class TempDir
{
public:
    TempDir()
    {
        std::random_device rd;
        std::mt19937_64 gen(rd());
        mPath = std::filesystem::temp_directory_path() / ("loglib_tcp_tls_" + std::to_string(gen()));
        std::filesystem::create_directories(mPath);
    }
    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(mPath, ec);
    }
    TempDir(const TempDir &) = delete;
    TempDir &operator=(const TempDir &) = delete;
    [[nodiscard]] std::filesystem::path File(const std::string &name) const
    {
        return mPath / name;
    }

private:
    std::filesystem::path mPath;
};

/// Generate a 2048-bit RSA key + self-signed cert valid for one year
/// and write both to PEM files. Throws on any OpenSSL failure;
/// individual stages call into raw OpenSSL APIs to keep the test
/// dependency-free of higher-level wrappers.
void GenerateSelfSignedCert(const std::filesystem::path &certPath, const std::filesystem::path &keyPath)
{
    EVP_PKEY *pkey = EVP_RSA_gen(2048);
    if (!pkey)
    {
        throw std::runtime_error("EVP_RSA_gen failed");
    }

    X509 *x509 = X509_new();
    if (!x509)
    {
        EVP_PKEY_free(pkey);
        throw std::runtime_error("X509_new failed");
    }

    X509_set_version(x509, 2); // X509 v3
    ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
    X509_gmtime_adj(X509_getm_notBefore(x509), 0);
    X509_gmtime_adj(X509_getm_notAfter(x509), 60L * 60L * 24L * 365L);
    X509_set_pubkey(x509, pkey);

    X509_NAME *name = X509_get_subject_name(x509);
    X509_NAME_add_entry_by_txt(
        name, "C", MBSTRING_ASC, reinterpret_cast<const unsigned char *>("US"), -1, -1, 0
    );
    X509_NAME_add_entry_by_txt(
        name,
        "O",
        MBSTRING_ASC,
        reinterpret_cast<const unsigned char *>("loglib-test"),
        -1,
        -1,
        0
    );
    X509_NAME_add_entry_by_txt(
        name,
        "CN",
        MBSTRING_ASC,
        reinterpret_cast<const unsigned char *>("localhost"),
        -1,
        -1,
        0
    );
    X509_set_issuer_name(x509, name);

    if (!X509_sign(x509, pkey, EVP_sha256()))
    {
        X509_free(x509);
        EVP_PKEY_free(pkey);
        throw std::runtime_error("X509_sign failed");
    }

    FILE *certFp = std::fopen(certPath.string().c_str(), "wb");
    if (!certFp)
    {
        X509_free(x509);
        EVP_PKEY_free(pkey);
        throw std::runtime_error("open cert file for write failed");
    }
    PEM_write_X509(certFp, x509);
    std::fclose(certFp);

    FILE *keyFp = std::fopen(keyPath.string().c_str(), "wb");
    if (!keyFp)
    {
        X509_free(x509);
        EVP_PKEY_free(pkey);
        throw std::runtime_error("open key file for write failed");
    }
    PEM_write_PrivateKey(keyFp, pkey, nullptr, nullptr, 0, nullptr, nullptr);
    std::fclose(keyFp);

    X509_free(x509);
    EVP_PKEY_free(pkey);
}

std::string DrainUntil(TcpServerProducer &producer, size_t target, std::chrono::milliseconds budget)
{
    std::string buf;
    std::array<char, 4096> chunk{};
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (buf.size() < target && std::chrono::steady_clock::now() < deadline)
    {
        producer.WaitForBytes(50ms);
        for (;;)
        {
            const size_t n = producer.Read(std::span<char>(chunk));
            if (n == 0)
            {
                break;
            }
            buf.append(chunk.data(), n);
        }
    }
    return buf;
}

} // namespace

TEST_CASE("TcpServerProducer (TLS): plaintext client cannot exchange data", "[tcp_producer][tls]")
{
    TempDir tmp;
    const auto certPath = tmp.File("cert.pem");
    const auto keyPath = tmp.File("key.pem");
    GenerateSelfSignedCert(certPath, keyPath);

    TcpServerProducer::Options opts;
    opts.bindAddress = "127.0.0.1";
    opts.tls.emplace();
    opts.tls->certificateChain = certPath;
    opts.tls->privateKey = keyPath;
    TcpServerProducer producer(opts);

    REQUIRE(producer.DisplayName().starts_with("tcp+tls://"));

    // A plaintext client can connect (the listener is a plain TCP
    // socket up until the TLS handshake), but no log bytes ever
    // surface because the handshake silently rejects garbage.
    test_common::TcpLogClient plain("127.0.0.1", producer.BoundPort());
    try
    {
        plain.Send("plaintext-into-tls\n");
    }
    catch (const std::exception &)
    {
    }
    const std::string drained = DrainUntil(producer, /*target*/ 1, ScaledMs(500ms));
    REQUIRE(drained.empty());
}

TEST_CASE("TcpServerProducer (TLS): TLS client send-receive with self-signed cert", "[tcp_producer][tls]")
{
    TempDir tmp;
    const auto certPath = tmp.File("cert.pem");
    const auto keyPath = tmp.File("key.pem");
    GenerateSelfSignedCert(certPath, keyPath);

    TcpServerProducer::Options opts;
    opts.bindAddress = "127.0.0.1";
    opts.tls.emplace();
    opts.tls->certificateChain = certPath;
    opts.tls->privateKey = keyPath;
    TcpServerProducer producer(opts);

    test_common::TcpLogClient::TlsOptions tls;
    tls.insecureSkipVerify = true; // self-signed
    test_common::TcpLogClient client("127.0.0.1", producer.BoundPort(), tls);
    client.Send("encrypted-line\n");

    const std::string drained = DrainUntil(producer, /*target*/ 15, ScaledMs(3000ms));
    REQUIRE(drained == "encrypted-line\n");
    REQUIRE(producer.TotalClientsAccepted() == 1);
}

TEST_CASE("TcpServerProducer (TLS): missing cert file fails fast in ctor", "[tcp_producer][tls]")
{
    TcpServerProducer::Options opts;
    opts.bindAddress = "127.0.0.1";
    opts.tls.emplace();
    opts.tls->certificateChain = std::filesystem::temp_directory_path() / "definitely-not-here.pem";
    opts.tls->privateKey = std::filesystem::temp_directory_path() / "definitely-not-here.key";
    REQUIRE_THROWS_AS(TcpServerProducer(opts), std::runtime_error);
}
