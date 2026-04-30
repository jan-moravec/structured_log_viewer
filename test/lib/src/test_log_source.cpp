#include "common.hpp"

#include <loglib/log_file.hpp>
#include <loglib/mapped_file_source.hpp>

#include <catch2/catch_all.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

using loglib::LogFile;
using loglib::MappedFileSource;

TEST_CASE("MappedFileSource Read returns the full mmap once and then 0 with IsClosed true", "[LogSource]")
{
    TestLogFile testLogFile;
    const std::string content = "Line 1\nLine 2\nLine 3\n";
    testLogFile.Write(content);
    auto logFile = testLogFile.CreateLogFile();
    LogFile *raw = logFile.get();

    SECTION("owning ctor")
    {
        MappedFileSource source(std::move(logFile));

        REQUIRE_FALSE(source.IsClosed());
        REQUIRE(source.IsMappedFile());
        REQUIRE(source.GetMappedLogFile() == raw);

        std::vector<char> buffer(content.size());
        const size_t read = source.Read(std::span<char>(buffer));
        CHECK(read == content.size());
        CHECK(std::string(buffer.data(), read) == content);

        // Subsequent Read should report terminal EOF without writing.
        std::vector<char> tail(64);
        CHECK(source.Read(std::span<char>(tail)) == 0);
        CHECK(source.IsClosed());

        // WaitForBytes is a no-op on finite mmap sources; calling it after
        // EOF must not block.
        source.WaitForBytes(std::chrono::milliseconds(50));
    }

    SECTION("borrowing ctor")
    {
        MappedFileSource source(*raw);

        std::vector<char> buffer(content.size());
        const size_t read = source.Read(std::span<char>(buffer));
        CHECK(read == content.size());
        CHECK(std::string(buffer.data(), read) == content);
        CHECK(source.IsClosed());
    }
}

TEST_CASE("MappedFileSource splits Read across multiple buffers", "[LogSource]")
{
    TestLogFile testLogFile;
    const std::string content = "abcdefghijklmnopqrstuvwxyz";
    testLogFile.Write(content);
    auto logFile = testLogFile.CreateLogFile();

    MappedFileSource source(std::move(logFile));

    std::array<char, 7> buffer{};
    std::string accumulated;
    while (!source.IsClosed())
    {
        const size_t read = source.Read(std::span<char>(buffer));
        if (read == 0)
        {
            break;
        }
        accumulated.append(buffer.data(), read);
    }

    CHECK(accumulated == content);
    CHECK(source.IsClosed());
}

TEST_CASE("MappedFileSource::Stop from another thread causes the next Read to report terminal EOF", "[LogSource]")
{
    TestLogFile testLogFile;
    // Large enough that the loop below has a chance to observe the stop
    // mid-read on a slow runner; the actual race is verified by the
    // terminal-EOF assertion below.
    std::string content;
    content.reserve(64 * 1024);
    for (size_t i = 0; i < 64 * 1024; ++i)
    {
        content.push_back(static_cast<char>('A' + (i % 26)));
    }
    testLogFile.Write(content);
    auto logFile = testLogFile.CreateLogFile();

    MappedFileSource source(std::move(logFile));

    std::atomic<bool> stopRequested{false};
    std::thread stopper([&] {
        // Yield once to give the reader a head-start; the test is robust
        // either way (stop-before-Read and stop-after-some-Reads both end
        // in IsClosed() == true with the next Read returning 0).
        std::this_thread::yield();
        source.Stop();
        stopRequested.store(true, std::memory_order_release);
    });

    std::array<char, 1024> buffer{};
    size_t totalRead = 0;
    while (true)
    {
        const size_t read = source.Read(std::span<char>(buffer));
        if (read == 0)
        {
            break;
        }
        totalRead += read;
    }

    stopper.join();
    REQUIRE(stopRequested.load(std::memory_order_acquire));

    // After Stop is observed, the source must report terminal EOF on every
    // subsequent Read regardless of how much was consumed before.
    std::array<char, 16> tail{};
    CHECK(source.Read(std::span<char>(tail)) == 0);
    CHECK(source.IsClosed());
    CHECK(totalRead <= content.size());
}

TEST_CASE("MappedFileSource::DisplayName matches the underlying file path", "[LogSource]")
{
    TestLogFile testLogFile;
    testLogFile.Write("x\n");
    auto logFile = testLogFile.CreateLogFile();

    MappedFileSource source(std::move(logFile));
    CHECK(source.DisplayName() == testLogFile.GetFilePath());
}

TEST_CASE("MappedFileSource::Stop is idempotent", "[LogSource]")
{
    TestLogFile testLogFile;
    testLogFile.Write("hello\n");
    auto logFile = testLogFile.CreateLogFile();

    MappedFileSource source(std::move(logFile));
    source.Stop();
    source.Stop(); // second call is a no-op

    std::array<char, 8> buffer{};
    CHECK(source.Read(std::span<char>(buffer)) == 0);
    CHECK(source.IsClosed());
}

TEST_CASE("MappedFileSource exposes its LogFile only while not stopped", "[LogSource]")
{
    TestLogFile testLogFile;
    testLogFile.Write("x\n");
    auto logFile = testLogFile.CreateLogFile();
    LogFile *raw = logFile.get();

    MappedFileSource source(std::move(logFile));
    CHECK(source.IsMappedFile());
    CHECK(source.GetMappedLogFile() == raw);

    source.Stop();

    // After Stop, the capability flag flips off so the parser bypasses the
    // mmap fast path (preserving the contract that Stop releases I/O even
    // for finite mmap sources, mirroring the live-tail teardown order).
    CHECK_FALSE(source.IsMappedFile());
    CHECK(source.GetMappedLogFile() == nullptr);
}

TEST_CASE("MappedFileSource on an empty file is closed immediately", "[LogSource]")
{
    TestLogFile testLogFile;
    testLogFile.Write("");
    auto logFile = std::make_unique<LogFile>(testLogFile.GetFilePath());

    MappedFileSource source(std::move(logFile));

    std::array<char, 16> buffer{};
    CHECK(source.Read(std::span<char>(buffer)) == 0);
    CHECK(source.IsClosed());
}

TEST_CASE("MappedFileSource::ReleaseFile transfers ownership and closes the source", "[LogSource]")
{
    TestLogFile testLogFile;
    testLogFile.Write("hello\n");
    auto logFile = testLogFile.CreateLogFile();
    LogFile *raw = logFile.get();

    MappedFileSource source(std::move(logFile));

    auto released = source.ReleaseFile();
    REQUIRE(released != nullptr);
    CHECK(released.get() == raw);

    // After release, the source no longer exposes the file and reads
    // return terminal EOF.
    CHECK(source.IsClosed());
    CHECK_FALSE(source.IsMappedFile());
    CHECK(source.GetMappedLogFile() == nullptr);

    std::array<char, 8> buffer{};
    CHECK(source.Read(std::span<char>(buffer)) == 0);
}

TEST_CASE("MappedFileSource borrowing ctor returns empty unique_ptr from ReleaseFile", "[LogSource]")
{
    TestLogFile testLogFile;
    testLogFile.Write("hello\n");
    auto logFile = testLogFile.CreateLogFile();

    MappedFileSource source(*logFile);

    auto released = source.ReleaseFile();
    CHECK(released == nullptr);
    CHECK(source.IsClosed());
}
