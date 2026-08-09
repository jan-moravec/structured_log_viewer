#include <loglib/stdin_bytes_producer.hpp>

#include <loglib_test/scaled_ms.hpp>
#include <test_common/pipe.hpp>

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

using loglib::StdinBytesProducer;
using loglib::internal::StdinBytesProducerTestAccess;
using loglib_test::ScaledMs;
using test_common::Pipe;
using namespace std::chrono_literals;

namespace
{

/// Small-timeout drain helper. Reads from the producer until
/// @p predicate matches or @p deadline elapses. Returns the
/// accumulated bytes.
template <typename Predicate>
std::string DrainUntil(StdinBytesProducer &producer, std::chrono::milliseconds deadline, Predicate predicate)
{
    std::string accumulated;
    const auto start = std::chrono::steady_clock::now();
    std::array<char, 4096> buf{};

    while (true)
    {
        const auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed >= deadline)
        {
            break;
        }
        const std::size_t got = producer.Read(std::span<char>(buf));
        if (got > 0)
        {
            accumulated.append(buf.data(), got);
            if (predicate(accumulated))
            {
                return accumulated;
            }
            continue;
        }
        if (predicate(accumulated))
        {
            return accumulated;
        }
        if (producer.IsClosed())
        {
            // Final drain; the worker signalled closed but bytes
            // may still be queued.
            while (true)
            {
                const std::size_t tail = producer.Read(std::span<char>(buf));
                if (tail == 0)
                {
                    break;
                }
                accumulated.append(buf.data(), tail);
            }
            break;
        }
        const auto remaining = deadline - elapsed;
        producer.WaitForBytes(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::min<std::chrono::steady_clock::duration>(remaining, ScaledMs(25ms))
            )
        );
    }
    return accumulated;
}

} // namespace

TEST_CASE("StdinBytesProducer delivers pipe bytes end-to-end", "[StdinBytesProducer]")
{
    Pipe pipe;
    auto producer = StdinBytesProducerTestAccess::Create(pipe.TakeReadEndOpaque(), {});
    REQUIRE(producer != nullptr);

    pipe.Write("hello\n");
    pipe.Write("world\n");
    pipe.CloseWrite();

    const std::string drained =
        DrainUntil(*producer, ScaledMs(2000ms), [](const std::string &acc) { return acc.contains("world\n"); });
    CHECK(drained == "hello\nworld\n");
}

TEST_CASE("StdinBytesProducer marks closed after write-end EOF", "[StdinBytesProducer]")
{
    Pipe pipe;
    auto producer = StdinBytesProducerTestAccess::Create(pipe.TakeReadEndOpaque(), {});
    REQUIRE(producer != nullptr);

    pipe.Write("chunk\n");
    pipe.CloseWrite();

    // Drain until the producer signals EOF; give the worker a
    // generous budget on slow CI. Once IsClosed() flips, the
    // Read call becomes a synchronous no-op.
    const auto deadline = std::chrono::steady_clock::now() + ScaledMs(2000ms);
    while (!producer->IsClosed() && std::chrono::steady_clock::now() < deadline)
    {
        producer->WaitForBytes(ScaledMs(25ms));
    }
    CHECK(producer->IsClosed());
}

TEST_CASE("StdinBytesProducer honours a synthetic display name", "[StdinBytesProducer]")
{
    Pipe pipe;
    StdinBytesProducer::Options options;
    options.displayName = "test-pipe";
    auto producer = StdinBytesProducerTestAccess::Create(pipe.TakeReadEndOpaque(), std::move(options));

    CHECK(producer->DisplayName() == "test-pipe");
}

TEST_CASE("StdinBytesProducer Stop() unblocks a pending WaitForBytes", "[StdinBytesProducer]")
{
    Pipe pipe;
    auto producer = StdinBytesProducerTestAccess::Create(pipe.TakeReadEndOpaque(), {});
    REQUIRE(producer != nullptr);

    // Start a waiter on a background thread. Without `Stop()`
    // this would park until either the write-end sends bytes
    // or the wait-for timeout expires (whichever comes first).
    // The test enforces the "wakes up fast" property directly.
    std::atomic<bool> waiterReturned{false};
    std::thread waiter([&] {
        producer->WaitForBytes(ScaledMs(5000ms));
        waiterReturned.store(true, std::memory_order_release);
    });

    // Give the waiter a moment to actually park.
    std::this_thread::sleep_for(ScaledMs(20ms));
    producer->Stop();

    const auto deadline = std::chrono::steady_clock::now() + ScaledMs(1000ms);
    while (!waiterReturned.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(ScaledMs(5ms));
    }
    if (waiter.joinable())
    {
        waiter.join();
    }
    CHECK(waiterReturned.load(std::memory_order_acquire));
    CHECK(producer->IsClosed());
}

TEST_CASE("StdinBytesProducer Stop is idempotent", "[StdinBytesProducer]")
{
    Pipe pipe;
    auto producer = StdinBytesProducerTestAccess::Create(pipe.TakeReadEndOpaque(), {});

    producer->Stop();
    producer->Stop();
    CHECK(producer->IsClosed());
}

TEST_CASE("StdinBytesProducer Stop is safe under concurrent callers", "[StdinBytesProducer]")
{
    // Regression: two threads racing `Stop()` used to both call
    // `mWorker.join()` on the CAS-loser branch (UB). Peers now
    // park on a "stop finished" latch until the CAS winner
    // publishes it, so all peer returns imply a fully quiesced
    // producer.
    constexpr int PEER_COUNT = 8;
    Pipe pipe;
    auto producer = StdinBytesProducerTestAccess::Create(pipe.TakeReadEndOpaque(), {});
    REQUIRE(producer != nullptr);

    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> peers;
    peers.reserve(PEER_COUNT);
    for (int i = 0; i < PEER_COUNT; ++i)
    {
        peers.emplace_back([&] {
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }
            producer->Stop();
        });
    }

    while (ready.load(std::memory_order_acquire) < PEER_COUNT)
    {
        std::this_thread::yield();
    }
    go.store(true, std::memory_order_release);

    for (auto &t : peers)
    {
        t.join();
    }

    // Every peer's `Stop()` return has to imply the producer is
    // observably closed, regardless of which thread won the CAS.
    CHECK(producer->IsClosed());
}
