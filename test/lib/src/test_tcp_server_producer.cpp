#include <loglib/tcp_server_producer.hpp>

#include <loglib_test/scaled_ms.hpp>
#include <test_common/network_log_client.hpp>

#include <catch2/catch_all.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <span>
#include <string>
#include <thread>

using loglib::TcpServerProducer;
using loglib_test::ScaledMs;
using namespace std::chrono_literals;

namespace
{

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

/// Wait until @p predicate holds or the budget expires. Used for
/// shaping the test loop around `ActiveClientCount` / `IsClosed`.
template <class Pred> bool WaitFor(Pred predicate, std::chrono::milliseconds budget)
{
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (predicate())
        {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return predicate();
}

} // namespace

TEST_CASE("TcpServerProducer: single client send-receive", "[tcp_producer]")
{
    TcpServerProducer::Options opts;
    opts.bindAddress = "127.0.0.1";
    TcpServerProducer producer(opts);
    REQUIRE(producer.BoundPort() != 0);

    test_common::TcpLogClient client("127.0.0.1", producer.BoundPort());
    client.Send("hello\n");
    client.Send("world\n");

    const std::string drained = DrainUntil(producer, /*target*/ 12, ScaledMs(2000ms));
    REQUIRE(drained == "hello\nworld\n");
}

TEST_CASE("TcpServerProducer: per-session carry preserves line boundaries with two clients", "[tcp_producer]")
{
    TcpServerProducer::Options opts;
    opts.bindAddress = "127.0.0.1";
    TcpServerProducer producer(opts);

    test_common::TcpLogClient a("127.0.0.1", producer.BoundPort());
    test_common::TcpLogClient b("127.0.0.1", producer.BoundPort());

    // Write each line in two halves (no trailing newline yet) so a
    // naive "raw concat" producer would torn-line this. The
    // per-session carry must hold each peer's partial until the
    // newline lands. Use `SendRaw` so the test_common helper does
    // not auto-append a newline -- that would defeat the test.
    a.SendRaw("AAAA");
    b.SendRaw("BBBB");
    a.SendRaw("AAAA\n"); // peer A's line completes
    b.SendRaw("BBBB\n"); // peer B's line completes

    const std::string drained = DrainUntil(producer, /*target*/ 18, ScaledMs(2000ms));
    INFO("drained = '" << drained << "'");
    // We don't pin the order of the two peers' lines (depends on
    // accept order + read scheduling), but each must be intact.
    REQUIRE((drained == "AAAAAAAA\nBBBBBBBB\n" || drained == "BBBBBBBB\nAAAAAAAA\n"));
}

TEST_CASE("TcpServerProducer: client disconnect flushes partial-line carry", "[tcp_producer]")
{
    TcpServerProducer::Options opts;
    opts.bindAddress = "127.0.0.1";
    TcpServerProducer producer(opts);

    {
        test_common::TcpLogClient client("127.0.0.1", producer.BoundPort());
        client.Send("partial-without-newline");
    } // client destruction => half-close => server sees EOF + finalises session

    const std::string drained = DrainUntil(producer, /*target*/ 24, ScaledMs(2000ms));
    REQUIRE(drained == "partial-without-newline\n");
    REQUIRE(WaitFor([&] { return producer.ActiveClientCount() == 0; }, ScaledMs(2000ms)));
    REQUIRE(producer.TotalClientsAccepted() == 1);
}

TEST_CASE("TcpServerProducer: accept cap rejects extra connections", "[tcp_producer]")
{
    TcpServerProducer::Options opts;
    opts.bindAddress = "127.0.0.1";
    opts.maxConcurrentClients = 2;
    TcpServerProducer producer(opts);

    const test_common::TcpLogClient a("127.0.0.1", producer.BoundPort());
    const test_common::TcpLogClient b("127.0.0.1", producer.BoundPort());
    REQUIRE(WaitFor([&] { return producer.ActiveClientCount() == 2; }, ScaledMs(2000ms)));

    // Third connection: the producer accepts and immediately closes
    // it, so any send racing the close may or may not raise -- we
    // only care that the rejection counter ticks and the client
    // observes the closed connection.
    try
    {
        test_common::TcpLogClient c("127.0.0.1", producer.BoundPort());
        // Best-effort send; the connection is half-open (server-side
        // closed). Don't assert on success here.
        try
        {
            c.Send("rejected\n");
        }
        catch (const std::exception &)
        {
        }
    }
    catch (const std::exception &)
    {
    }

    REQUIRE(WaitFor([&] { return producer.TotalClientsRejected() >= 1; }, ScaledMs(2000ms)));
    REQUIRE(producer.ActiveClientCount() == 2);
}

TEST_CASE("TcpServerProducer: Stop() unblocks WaitForBytes", "[tcp_producer]")
{
    TcpServerProducer::Options opts;
    opts.bindAddress = "127.0.0.1";
    TcpServerProducer producer(opts);

    std::atomic<bool> woke{false};
    std::thread waiter([&] {
        producer.WaitForBytes(ScaledMs(5000ms));
        woke = true;
    });
    std::this_thread::sleep_for(ScaledMs(50ms));
    producer.Stop();
    waiter.join();
    REQUIRE(woke.load());
    producer.Stop(); // idempotent
}

TEST_CASE("TcpServerProducer: DisplayName starts with tcp:// scheme", "[tcp_producer]")
{
    TcpServerProducer::Options opts;
    opts.bindAddress = "127.0.0.1";
    TcpServerProducer producer(opts);
    const std::string display = producer.DisplayName();
    REQUIRE(display.starts_with("tcp://127.0.0.1:"));
    REQUIRE(display.ends_with(std::to_string(producer.BoundPort())));
}

TEST_CASE("TcpServerProducer: invalid bind address throws", "[tcp_producer]")
{
    TcpServerProducer::Options opts;
    opts.bindAddress = "not-a-host";
    REQUIRE_THROWS_AS(TcpServerProducer(opts), std::system_error);
}
