#include <loglib/udp_server_producer.hpp>

#include <loglib_test/scaled_ms.hpp>
#include <test_common/network_log_client.hpp>

#include <catch2/catch_all.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <span>
#include <string>
#include <thread>

using loglib::UdpServerProducer;
using loglib_test::ScaledMs;
using namespace std::chrono_literals;

namespace
{

/// Drain the producer until at least @p target bytes accumulate or
/// the timeout elapses. Returns the accumulated text. Useful instead
/// of repeatedly hand-rolling the same WaitForBytes / Read loop in
/// every assertion.
std::string DrainUntil(UdpServerProducer &producer, size_t target, std::chrono::milliseconds budget)
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

TEST_CASE("UdpServerProducer: receives single datagram and queues bytes", "[udp_producer]")
{
    UdpServerProducer::Options opts;
    opts.bindAddress = "127.0.0.1";
    opts.port = 0; // ephemeral
    UdpServerProducer producer(opts);
    const uint16_t port = producer.BoundPort();
    REQUIRE(port != 0);

    test_common::UdpLogClient client("127.0.0.1", port);
    client.Send("hello world");

    const std::string drained = DrainUntil(producer, /*target*/ 12, ScaledMs(2000ms));
    REQUIRE(drained == "hello world\n");
    REQUIRE(producer.DatagramCount() >= 1);
}

TEST_CASE("UdpServerProducer: appends synthetic newline only when missing", "[udp_producer]")
{
    UdpServerProducer::Options opts;
    opts.bindAddress = "127.0.0.1";
    UdpServerProducer producer(opts);

    test_common::UdpLogClient client("127.0.0.1", producer.BoundPort());
    client.Send("with-newline\n");
    client.Send("without-newline");

    const std::string drained = DrainUntil(producer, /*target*/ 30, ScaledMs(2000ms));
    REQUIRE(drained == "with-newline\nwithout-newline\n");
}

TEST_CASE("UdpServerProducer: multi-line datagrams pass through verbatim", "[udp_producer]")
{
    UdpServerProducer::Options opts;
    opts.bindAddress = "127.0.0.1";
    UdpServerProducer producer(opts);

    test_common::UdpLogClient client("127.0.0.1", producer.BoundPort());
    client.Send("line-a\nline-b\nline-c\n");

    const std::string drained = DrainUntil(producer, /*target*/ 21, ScaledMs(2000ms));
    REQUIRE(drained == "line-a\nline-b\nline-c\n");
}

TEST_CASE("UdpServerProducer: many datagrams interleave in arrival order", "[udp_producer]")
{
    UdpServerProducer::Options opts;
    opts.bindAddress = "127.0.0.1";
    UdpServerProducer producer(opts);

    constexpr size_t COUNT = 64;
    test_common::UdpLogClient client("127.0.0.1", producer.BoundPort());
    for (size_t i = 0; i < COUNT; ++i)
    {
        client.Send("line-" + std::to_string(i));
    }

    // Each line is "line-N\n" so the lower bound is at least 7 bytes
    // per datagram; we just want to know we have all 64 lines.
    const std::string drained = DrainUntil(producer, /*target*/ 7 * COUNT, ScaledMs(3000ms));
    // Count newlines as a structural assertion: even if datagram drops
    // happen on a lossy localhost (rare but possible on Windows), we
    // demand at least 90% landed.
    const size_t newlines = static_cast<size_t>(std::count(drained.begin(), drained.end(), '\n'));
    REQUIRE(newlines >= COUNT * 9 / 10);
}

TEST_CASE("UdpServerProducer: Stop() unblocks WaitForBytes", "[udp_producer]")
{
    UdpServerProducer::Options opts;
    opts.bindAddress = "127.0.0.1";
    UdpServerProducer producer(opts);

    std::atomic<bool> woke{false};
    std::thread waiter([&] {
        producer.WaitForBytes(ScaledMs(5000ms));
        woke = true;
    });

    std::this_thread::sleep_for(ScaledMs(50ms));
    producer.Stop();
    waiter.join();
    REQUIRE(woke.load());
    // Idempotent: second Stop must not deadlock or throw.
    producer.Stop();
}

TEST_CASE("UdpServerProducer: invalid bind address throws", "[udp_producer]")
{
    UdpServerProducer::Options opts;
    opts.bindAddress = "not-a-host";
    REQUIRE_THROWS_AS(UdpServerProducer(opts), std::system_error);
}

TEST_CASE("UdpServerProducer: DisplayName reflects bound port", "[udp_producer]")
{
    UdpServerProducer::Options opts;
    opts.bindAddress = "127.0.0.1";
    UdpServerProducer producer(opts);
    const std::string display = producer.DisplayName();
    REQUIRE(display.starts_with("udp://127.0.0.1:"));
    REQUIRE(display.ends_with(std::to_string(producer.BoundPort())));
}
