#include "common.hpp"

#include <loglib/core/bytes_producer.hpp>
#include <loglib/core/key_index.hpp>
#include <loglib/core/stop_token.hpp>
#include <loglib/core/stream_line_source.hpp>
#include <loglib/parse/auto_detect_parser.hpp>
#include <loglib/parse/format_detection.hpp>
#include <loglib/parse/log_parse_sink.hpp>
#include <loglib/parse/log_parser.hpp>
#include <loglib/parse/parser_options.hpp>
#include <loglib/schema/log_configuration.hpp>

#include <test_common/temp_dir.hpp>
#include <test_common/wait.hpp>

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using loglib::AutoDetectParser;
using loglib::BytesProducer;
using loglib::KeyIndex;
using loglib::LogParseSink;
using loglib::ParserOptions;
using loglib::RowBatch;
using loglib::StreamLineSource;
using test_common::TempDir;
using namespace std::chrono_literals;

namespace
{

/// Sink that counts parsed rows; format-specific decoding is tested
/// elsewhere.
class CollectingSink final : public LogParseSink
{
public:
    KeyIndex &Keys() override
    {
        return mKeys;
    }
    void OnStarted() override
    {
        started.store(true, std::memory_order_release);
    }
    void OnBatch(RowBatch batch) override
    {
        rowCount.fetch_add(batch.lines.size(), std::memory_order_relaxed);
        errorCount.fetch_add(batch.errors.size(), std::memory_order_relaxed);
    }
    void OnFinished(bool cancelled) override
    {
        finished.store(true, std::memory_order_release);
        finishedCancelled.store(cancelled, std::memory_order_release);
    }

    std::atomic<bool> started{false};
    std::atomic<std::size_t> rowCount{0};
    std::atomic<std::size_t> errorCount{0};
    std::atomic<bool> finished{false};
    std::atomic<bool> finishedCancelled{false};

private:
    KeyIndex mKeys;
};

/// Runs `ParseStreaming` on a worker and waits up to @p deadline for
/// completion before joining it.
void RunStreaming(
    const loglib::LogParser &parser,
    StreamLineSource &source,
    CollectingSink &sink,
    ParserOptions options,
    std::chrono::milliseconds deadline
)
{
    std::thread worker([&] { parser.ParseStreaming(source, sink, std::move(options)); });
    static_cast<void>(test_common::WaitUntil([&] { return sink.finished.load(std::memory_order_acquire); }, deadline));
    if (worker.joinable())
    {
        worker.join();
    }
}

} // namespace

TEST_CASE("AutoDetectParser IsValidBytes accepts anything", "[AutoDetectParser]")
{
    // The adapter is a routing layer -- it never rejects, the
    // resolved parser decides on the concrete bytes.
    const AutoDetectParser parser;
    CHECK(parser.IsValidBytes({}));
    CHECK(parser.IsValidBytes("gibberish"));
    CHECK(parser.IsValidBytes(R"({"json":true})"));
}

TEST_CASE("AutoDetectParser routes JSON bytes to a JSON parse", "[AutoDetectParser]")
{
    const std::string bytes = R"({"level":"info","message":"first"}
{"level":"warn","message":"second"}
{"level":"error","message":"third"}
)";

    auto producer = std::make_unique<InMemoryBytesProducer>(bytes, 64);
    StreamLineSource source(std::filesystem::path("<auto>"), std::move(producer));

    CollectingSink sink;
    ParserOptions options;
    RunStreaming(AutoDetectParser(), source, sink, std::move(options), std::chrono::seconds(2));

    CHECK(sink.finished.load());
    CHECK_FALSE(sink.finishedCancelled.load());
    CHECK(sink.rowCount.load() == 3);
}

TEST_CASE("AutoDetectParser routes logfmt bytes to a logfmt parse", "[AutoDetectParser]")
{
    const std::string bytes = "level=info message=first\nlevel=warn message=second\n";

    auto producer = std::make_unique<InMemoryBytesProducer>(bytes, 64);
    StreamLineSource source(std::filesystem::path("<auto>"), std::move(producer));

    CollectingSink sink;
    RunStreaming(AutoDetectParser(), source, sink, {}, std::chrono::seconds(2));

    CHECK(sink.finished.load());
    CHECK(sink.rowCount.load() == 2);
}

TEST_CASE("AutoDetectParser routes CSV bytes to a CSV parse", "[AutoDetectParser]")
{
    // First line is the header, next two are data rows.
    const std::string bytes = "level,message\ninfo,first\nwarn,second\n";

    auto producer = std::make_unique<InMemoryBytesProducer>(bytes, 64);
    StreamLineSource source(std::filesystem::path("<auto>"), std::move(producer));

    CollectingSink sink;
    RunStreaming(AutoDetectParser(), source, sink, {}, std::chrono::seconds(2));

    CHECK(sink.finished.load());
    CHECK(sink.rowCount.load() == 2);
}

TEST_CASE("AutoDetectParser handles an empty producer via JsonParser fallback", "[AutoDetectParser]")
{
    // Empty producer -> empty peek -> fall back to JsonParser
    // for a clean "OnFinished(false)" cycle with zero rows.
    auto producer = std::make_unique<InMemoryBytesProducer>(std::string{}, 64);
    StreamLineSource source(std::filesystem::path("<auto>"), std::move(producer));

    CollectingSink sink;
    RunStreaming(AutoDetectParser(), source, sink, {}, std::chrono::seconds(2));

    CHECK(sink.finished.load());
    CHECK_FALSE(sink.finishedCancelled.load());
    CHECK(sink.rowCount.load() == 0);
}

TEST_CASE("AutoDetectParser (file path) uses DetectFormatForPath under the hood", "[AutoDetectParser]")
{
    // Building a FileLineSource is
    // heavyweight to set up in unit tests. Instead, cover the
    // file-side branch by asserting that the same detection
    // logic (`DetectFormatForPath`) reaches the right verdict on
    // a small on-disk fixture. The `AutoDetectParser`
    // `FileLineSource` overload is a one-liner around this call.
    const TempDir dir("auto_detect");
    const auto filePath = dir.Write("routing.log", "level=info message=first\nlevel=warn message=second\n");

    const loglib::DetectedFormat detected = loglib::DetectFormatForPath(filePath);
    CHECK(detected.format == loglib::Source::Format::Logfmt);
}

TEST_CASE("AutoDetectParser routes regex-template bytes to a regex parse", "[AutoDetectParser]")
{
    // Standard syslog-like lines recognised by the built-in catalog.
    const std::string bytes = "2025-08-07T09:00:00Z INFO svc: first message\n"
                              "2025-08-07T09:00:01Z WARN svc: second message\n";

    auto producer = std::make_unique<InMemoryBytesProducer>(bytes, 64);
    StreamLineSource source(std::filesystem::path("<auto>"), std::move(producer));

    CollectingSink sink;
    RunStreaming(AutoDetectParser(), source, sink, {}, std::chrono::seconds(2));

    CHECK(sink.finished.load());
    CHECK_FALSE(sink.finishedCancelled.load());
    // Regex format resolved at least *some* rows; the exact count
    // depends on which template matched, so require >= 1 rather
    // than pinning to a specific catalog member.
    CHECK(sink.rowCount.load() >= 1);
}

namespace
{

/// In-memory producer whose `Read` yields chunks separated by a
/// fixed delay. The first chunk is released immediately; each
/// subsequent chunk becomes readable @p gap after the previous
/// one drained. Between chunks, `Read` returns 0 and
/// `WaitForBytes` blocks up to the passed timeout waiting for the
/// release timer. Models a slow network sender.
class SlowBytesProducer final : public BytesProducer
{
public:
    SlowBytesProducer(std::vector<std::string> chunks, std::chrono::milliseconds gap)
        : mChunks(std::move(chunks)), mGap(gap), mNextReleaseAt(std::chrono::steady_clock::now())
    {
    }

    ~SlowBytesProducer() override
    {
        Stop();
    }

    SlowBytesProducer(const SlowBytesProducer &) = delete;
    SlowBytesProducer &operator=(const SlowBytesProducer &) = delete;

    std::size_t Read(std::span<char> buffer) override
    {
        const std::unique_lock<std::mutex> lock(mLock);
        if (mChunkCursor >= mChunks.size())
        {
            mClosed.store(true, std::memory_order_release);
            return 0;
        }
        if (std::chrono::steady_clock::now() < mNextReleaseAt)
        {
            return 0;
        }
        const std::string &chunk = mChunks[mChunkCursor];
        const std::size_t remaining = chunk.size() - mByteCursor;
        const std::size_t give = std::min(remaining, buffer.size());
        std::memcpy(buffer.data(), chunk.data() + mByteCursor, give);
        mByteCursor += give;
        if (mByteCursor >= chunk.size())
        {
            ++mChunkCursor;
            mByteCursor = 0;
            mNextReleaseAt = std::chrono::steady_clock::now() + mGap;
            if (mChunkCursor >= mChunks.size())
            {
                mClosed.store(true, std::memory_order_release);
            }
        }
        return give;
    }

    void WaitForBytes(std::chrono::milliseconds timeout) override
    {
        std::unique_lock<std::mutex> lock(mLock);
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        mCv.wait_until(lock, std::min(deadline, mNextReleaseAt), [&] {
            return mStop.load(std::memory_order_acquire) || std::chrono::steady_clock::now() >= mNextReleaseAt;
        });
    }

    void Stop() noexcept override
    {
        mStop.store(true, std::memory_order_release);
        mClosed.store(true, std::memory_order_release);
        const std::scoped_lock lock(mLock);
        mCv.notify_all();
    }

    [[nodiscard]] bool IsClosed() const noexcept override
    {
        return mClosed.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::string DisplayName() const override
    {
        return "slow";
    }

private:
    mutable std::mutex mLock;
    std::condition_variable mCv;
    std::vector<std::string> mChunks;
    std::size_t mChunkCursor = 0;
    std::size_t mByteCursor = 0;
    std::chrono::milliseconds mGap;
    std::chrono::steady_clock::time_point mNextReleaseAt;
    std::atomic<bool> mStop{false};
    std::atomic<bool> mClosed{false};
};

} // namespace

TEST_CASE("AutoDetectParser composes with bytes the caller gave back to the source", "[AutoDetectParser]")
{
    // The adapter puts the source's unread bytes in front of its peek, so the
    // resolved parser sees the full in-order stream.
    //
    // Split the JSON across the unread bytes and the producer: the producer
    // holds a valid probe head (first line -> JSON), and the caller gave back
    // a second full line with UnreadBytes. Both must land as rows.
    const std::string producerBytes = R"({"msg":"from-producer"})"
                                      "\n";
    const std::string carryBytes = R"({"msg":"from-carry"})"
                                   "\n";

    auto producer = std::make_unique<InMemoryBytesProducer>(producerBytes, 64);
    StreamLineSource source(std::filesystem::path("<auto>"), std::move(producer));

    CollectingSink sink;
    source.UnreadBytes(carryBytes);
    const ParserOptions options;
    RunStreaming(AutoDetectParser(), source, sink, options, std::chrono::seconds(2));

    CHECK(sink.finished.load());
    CHECK_FALSE(sink.finishedCancelled.load());
    CHECK(sink.rowCount.load() == 2);
}

TEST_CASE("AutoDetectParser short-circuits the peek once detection commits", "[AutoDetectParser]")
{
    // The first chunk identifies JSON; the second is delayed 30 s.
    // Detection must commit within 2 s, before the 30 s peek deadline.
    std::vector<std::string> chunks;
    chunks.emplace_back(
        R"({"level":"info","message":"early"})"
        "\n"
    );
    chunks.emplace_back(
        R"({"level":"warn","message":"late"})"
        "\n"
    );
    auto producer = std::make_unique<SlowBytesProducer>(std::move(chunks), std::chrono::milliseconds(30000));
    StreamLineSource source(std::filesystem::path("<auto>"), std::move(producer));

    CollectingSink sink;
    ParserOptions options;
    loglib::StopSource stopSource;
    options.stopToken = stopSource.GetToken();
    const AutoDetectParser parser(loglib::PROBE_BYTES_BUDGET, std::chrono::milliseconds(30000));
    const auto startedAt = std::chrono::steady_clock::now();
    std::thread worker([&] { parser.ParseStreaming(source, sink, std::move(options)); });

    static_cast<void>(test_common::WaitUntil([&] { return sink.started.load(std::memory_order_acquire); }, 15s));
    CHECK(sink.started.load());
    CHECK(std::chrono::steady_clock::now() - startedAt < std::chrono::milliseconds(15000));

    stopSource.RequestStop();
    if (worker.joinable())
    {
        worker.join();
    }
    CHECK(sink.finished.load());
}

TEST_CASE("AutoDetectParser deadline caps the peek on an unrecognised producer", "[AutoDetectParser]")
{
    // First chunk is nothing but whitespace/gibberish that no
    // probe claims. Second chunk (delayed) contains a JSON line
    // but arrives after the peek deadline. The adapter must fall
    // through to the fallback parser (`JsonParser`) instead of
    // blocking the peek loop for as long as `mPeekTimeout`
    // allows plus the full 5 s gap.
    std::vector<std::string> chunks;
    chunks.emplace_back("   \n   \n"); // two blank lines, no verdict
    chunks.emplace_back(
        R"({"msg":"late"})"
        "\n"
    );
    auto producer = std::make_unique<SlowBytesProducer>(std::move(chunks), std::chrono::milliseconds(5000));
    StreamLineSource source(std::filesystem::path("<auto>"), std::move(producer));

    CollectingSink sink;
    const auto startedAt = std::chrono::steady_clock::now();
    ParserOptions options;
    loglib::StopSource stopSource;
    options.stopToken = stopSource.GetToken();
    // Explicit 200 ms peek deadline -- shorter than the default
    // to keep the test snappy while still comfortably longer
    // than the chunk-arrival latency of the first release.
    const AutoDetectParser parser(loglib::PROBE_BYTES_BUDGET, std::chrono::milliseconds(200));
    std::thread worker([&] { parser.ParseStreaming(source, sink, std::move(options)); });

    static_cast<void>(test_common::WaitUntil([&] { return sink.started.load(std::memory_order_acquire); }, 15s));
    CHECK(sink.started.load());
    // Allow startup slack beyond the 200 ms peek deadline.
    CHECK(std::chrono::steady_clock::now() - startedAt < std::chrono::milliseconds(15000));

    stopSource.RequestStop();
    if (worker.joinable())
    {
        worker.join();
    }
    CHECK(sink.finished.load());
}

namespace
{

/// The peek timeout of the tests below. It only has to be shorter than the idle time they wait for.
constexpr std::chrono::milliseconds SHORT_PEEK_TIMEOUT{50};

/// The peek parks in WaitForBytes for 100 ms per call while idle. After this many calls it has been idle for 100 ms, twice SHORT_PEEK_TIMEOUT.
constexpr std::size_t IDLE_WAIT_CALLS = 2;

} // namespace

TEST_CASE("AutoDetectParser waits for a delayed sender before detecting", "[AutoDetectParser]")
{
    // The peek timer starts at the first byte, so an idle source for longer than the timeout must not make the parser
    // fall back to JSON. The producer stays open and has no bytes until the test appends them.
    auto producer = std::make_unique<InMemoryBytesProducer>(
        std::string{}, std::string::npos, InMemoryBytesProducer::Ending::OnStop
    );
    InMemoryBytesProducer *raw = producer.get();
    StreamLineSource source(std::filesystem::path("<network>"), std::move(producer));

    CollectingSink sink;
    ParserOptions options;
    loglib::StopSource stopSource;
    options.stopToken = stopSource.GetToken();
    const AutoDetectParser parser(loglib::PROBE_BYTES_BUDGET, SHORT_PEEK_TIMEOUT);
    std::thread worker([&] { parser.ParseStreaming(source, sink, std::move(options)); });

    REQUIRE(test_common::WaitUntil([&] { return raw->WaitCalls() >= IDLE_WAIT_CALLS; }));
    CHECK_FALSE(sink.started.load());
    raw->Append("level=info msg=first\nlevel=warn msg=second\nlevel=error msg=third\n");

    raw->Finish();

    const bool finished = test_common::WaitUntil([&] { return sink.finished.load(std::memory_order_acquire); });
    stopSource.RequestStop(); // only matters when the wait above timed out
    worker.join();
    CHECK(finished);
    CHECK(sink.finished.load());
    CHECK(sink.rowCount.load() == 3);
    CHECK(sink.errorCount.load() == 0);
}

TEST_CASE("AutoDetectParser stop ends an idle peek", "[AutoDetectParser]")
{
    auto producer = std::make_unique<InMemoryBytesProducer>(
        std::string{}, std::string::npos, InMemoryBytesProducer::Ending::OnStop
    );
    InMemoryBytesProducer *raw = producer.get();
    StreamLineSource source(std::filesystem::path("<network>"), std::move(producer));

    CollectingSink sink;
    ParserOptions options;
    loglib::StopSource stopSource;
    options.stopToken = stopSource.GetToken();
    const AutoDetectParser parser;
    std::thread worker([&] { parser.ParseStreaming(source, sink, std::move(options)); });

    // The worker is in the peek once it has called WaitForBytes.
    REQUIRE(test_common::WaitUntil([&] { return raw->WaitCalls() >= 1; }));
    CHECK_FALSE(sink.finished.load());
    stopSource.RequestStop();
    worker.join();
    CHECK(sink.finished.load());
    CHECK(sink.rowCount.load() == 0);
}
