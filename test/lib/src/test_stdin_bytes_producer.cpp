#include <loglib/stdin_bytes_producer.hpp>

#include <loglib_test/scaled_ms.hpp>

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#endif

using loglib::StdinBytesProducer;
using loglib::internal::StdinBytesProducerTestAccess;
using loglib_test::ScaledMs;
using namespace std::chrono_literals;

namespace
{

/// RAII wrapper around a platform pipe. Owns the write-end;
/// hands the read-end over to `StdinBytesProducer` via
/// `StdinBytesProducerTestAccess::Create`. On teardown the
/// write-end is closed so any producer still attached observes
/// EOF and stops cleanly.
class Pipe
{
public:
    Pipe()
    {
#ifdef _WIN32
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = FALSE;
        HANDLE readEnd = nullptr;
        HANDLE writeEnd = nullptr;
        const BOOL ok = ::CreatePipe(&readEnd, &writeEnd, &sa, /*nSize=*/0);
        REQUIRE(ok != 0);
        mRead = readEnd;
        mWrite = writeEnd;
#else
        int fds[2] = {-1, -1};
        const int rc = ::pipe(fds);
        REQUIRE(rc == 0);
        mRead = fds[0];
        mWrite = fds[1];
#endif
    }

    // Teardown ignores errors because the test may already have
    // closed both ends manually.
    // NOLINTNEXTLINE(bugprone-exception-escape)
    ~Pipe() noexcept
    {
        CloseWrite();
        CloseRead();
    }

    Pipe(const Pipe &) = delete;
    Pipe &operator=(const Pipe &) = delete;

    /// Transfer the read-end to the producer; the pipe drops
    /// its own reference so `CloseRead` becomes a no-op.
    [[nodiscard]] void *TakeReadEndOpaque()
    {
#ifdef _WIN32
        HANDLE h = mRead;
        mRead = nullptr;
        return static_cast<void *>(h);
#else
        const int fd = mRead;
        mRead = -1;
        return reinterpret_cast<void *>(static_cast<std::intptr_t>(fd));
#endif
    }

    void Write(std::string_view bytes) const
    {
#ifdef _WIN32
        REQUIRE(mWrite != nullptr);
        DWORD written = 0;
        const BOOL ok =
            ::WriteFile(mWrite, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
        REQUIRE(ok != 0);
        REQUIRE(written == bytes.size());
#else
        REQUIRE(mWrite >= 0);
        const ::ssize_t got = ::write(mWrite, bytes.data(), bytes.size());
        REQUIRE(got == static_cast<::ssize_t>(bytes.size()));
#endif
    }

    void CloseWrite() noexcept
    {
#ifdef _WIN32
        if (mWrite != nullptr)
        {
            ::CloseHandle(mWrite);
            mWrite = nullptr;
        }
#else
        if (mWrite >= 0)
        {
            ::close(mWrite);
            mWrite = -1;
        }
#endif
    }

    void CloseRead() noexcept
    {
#ifdef _WIN32
        if (mRead != nullptr)
        {
            ::CloseHandle(mRead);
            mRead = nullptr;
        }
#else
        if (mRead >= 0)
        {
            ::close(mRead);
            mRead = -1;
        }
#endif
    }

private:
#ifdef _WIN32
    HANDLE mRead = nullptr;
    HANDLE mWrite = nullptr;
#else
    int mRead = -1;
    int mWrite = -1;
#endif
};

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
        producer.WaitForBytes(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::min<std::chrono::steady_clock::duration>(remaining, ScaledMs(25ms))
        ));
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

    const std::string drained = DrainUntil(*producer, ScaledMs(2000ms), [](const std::string &acc) {
        return acc.find("world\n") != std::string::npos;
    });
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
