#include <loglib/auto_detect_parser.hpp>
#include <loglib/bytes_producer.hpp>
#include <loglib/format_detection.hpp>
#include <loglib/key_index.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_parse_sink.hpp>
#include <loglib/log_parser.hpp>
#include <loglib/parser_options.hpp>
#include <loglib/stop_token.hpp>
#include <loglib/stream_line_source.hpp>

#include <test_common/temp_dir.hpp>

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
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
using loglib::StreamedBatch;
using loglib::StreamLineSource;
using test_common::TempDir;

namespace
{

/// Byte-array producer: hands the entire buffer to the parser in
/// small chunks, then reports EOF. Threaded, so `WaitForBytes`
/// eventually resolves without spinning the caller.
class BufferedBytesProducer final : public BytesProducer
{
public:
    explicit BufferedBytesProducer(std::string bytes, std::size_t chunkSize = 64)
        : mBytes(std::move(bytes)), mChunkSize(chunkSize)
    {
    }

    ~BufferedBytesProducer() override
    {
        Stop();
    }

    BufferedBytesProducer(const BufferedBytesProducer &) = delete;
    BufferedBytesProducer &operator=(const BufferedBytesProducer &) = delete;

    std::size_t Read(std::span<char> buffer) override
    {
        std::lock_guard<std::mutex> lock(mLock);
        const std::size_t remaining = mBytes.size() - mCursor;
        if (remaining == 0)
        {
            mClosed.store(true, std::memory_order_release);
            return 0;
        }
        const std::size_t give = std::min({remaining, buffer.size(), mChunkSize});
        std::memcpy(buffer.data(), mBytes.data() + mCursor, give);
        mCursor += give;
        if (mCursor >= mBytes.size())
        {
            mClosed.store(true, std::memory_order_release);
        }
        return give;
    }

    void WaitForBytes(std::chrono::milliseconds /*timeout*/) override
    {
        // Bytes are always available until the buffer drains, at
        // which point `IsClosed` flips. No parking required.
    }

    void Stop() noexcept override
    {
        mClosed.store(true, std::memory_order_release);
    }

    [[nodiscard]] bool IsClosed() const noexcept override
    {
        std::lock_guard<std::mutex> lock(mLock);
        return mClosed.load(std::memory_order_acquire) && mCursor >= mBytes.size();
    }

    [[nodiscard]] std::string DisplayName() const override
    {
        return "buffered";
    }

private:
    mutable std::mutex mLock;
    std::string mBytes;
    std::size_t mCursor = 0;
    std::size_t mChunkSize;
    std::atomic<bool> mClosed{false};
};

/// Sink that concatenates parsed row payloads. Simple string
/// aggregation is enough to assert "the resolved parser saw the
/// bytes"; format-specific decoding is covered elsewhere.
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
    void OnBatch(StreamedBatch batch) override
    {
        rowCount.fetch_add(batch.lines.size(), std::memory_order_relaxed);
    }
    void OnFinished(bool cancelled) override
    {
        finished.store(true, std::memory_order_release);
        finishedCancelled.store(cancelled, std::memory_order_release);
    }

    std::atomic<bool> started{false};
    std::atomic<std::size_t> rowCount{0};
    std::atomic<bool> finished{false};
    std::atomic<bool> finishedCancelled{false};

private:
    KeyIndex mKeys;
};

/// Runs `parser.ParseStreaming(source, sink, options)` on a
/// dedicated worker with a bounded wall-clock deadline. Returns
/// once the worker joins.
void RunStreaming(
    const loglib::LogParser &parser,
    StreamLineSource &source,
    CollectingSink &sink,
    ParserOptions options,
    std::chrono::milliseconds deadline
)
{
    std::thread worker([&] { parser.ParseStreaming(source, sink, std::move(options)); });
    const auto giveUpAt = std::chrono::steady_clock::now() + deadline;
    while (!sink.finished.load(std::memory_order_acquire))
    {
        if (std::chrono::steady_clock::now() >= giveUpAt)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
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

    auto producer = std::make_unique<BufferedBytesProducer>(bytes);
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

    auto producer = std::make_unique<BufferedBytesProducer>(bytes);
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

    auto producer = std::make_unique<BufferedBytesProducer>(bytes);
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
    auto producer = std::make_unique<BufferedBytesProducer>(std::string{});
    StreamLineSource source(std::filesystem::path("<auto>"), std::move(producer));

    CollectingSink sink;
    RunStreaming(AutoDetectParser(), source, sink, {}, std::chrono::seconds(2));

    CHECK(sink.finished.load());
    CHECK_FALSE(sink.finishedCancelled.load());
    CHECK(sink.rowCount.load() == 0);
}

TEST_CASE("AutoDetectParser (file path) uses DetectFormatForPath under the hood", "[AutoDetectParser]")
{
    // FileLineSource construction requires a LogFile which is
    // heavyweight to set up in unit tests. Instead, cover the
    // file-side branch by asserting that the same detection
    // logic (`DetectFormatForPath`) reaches the right verdict on
    // a small on-disk fixture. The `AutoDetectParser`
    // `FileLineSource` overload is a one-liner around this call.
    const TempDir dir("auto_detect");
    const auto filePath =
        dir.Write("routing.log", "level=info message=first\nlevel=warn message=second\n");

    const loglib::DetectedFormat detected = loglib::DetectFormatForPath(filePath);
    CHECK(detected.format == loglib::LogConfiguration::Source::Format::Logfmt);
}

TEST_CASE("AutoDetectParser routes regex-template bytes to a regex parse", "[AutoDetectParser]")
{
    // Standard syslog-ish header that the built-in regex catalog
    // recognises. Two record lines so we can assert row count.
    const std::string bytes =
        "2025-08-07T09:00:00Z INFO svc: first message\n"
        "2025-08-07T09:00:01Z WARN svc: second message\n";

    auto producer = std::make_unique<BufferedBytesProducer>(bytes);
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

TEST_CASE("AutoDetectParser composes with a caller-supplied initialCarry", "[AutoDetectParser]")
{
    // Regression: `ParseStreaming(StreamLineSource&, ...)` used to
    // clobber `options.initialCarry` when a caller pre-fed bytes.
    // Now the adapter prepends peek to any prior carry so the
    // resolved parser sees the full in-order stream.
    //
    // Split the JSON across the carry / producer boundary: the
    // producer holds a valid probe head (first line -> JSON), and
    // the caller pre-fed a second full line via `initialCarry`.
    // Both must land as rows.
    const std::string producerBytes = R"({"msg":"from-producer"})"
                                      "\n";
    const std::string carryBytes = R"({"msg":"from-carry"})"
                                   "\n";

    auto producer = std::make_unique<BufferedBytesProducer>(producerBytes);
    StreamLineSource source(std::filesystem::path("<auto>"), std::move(producer));

    CollectingSink sink;
    ParserOptions options;
    options.initialCarry = carryBytes;
    RunStreaming(AutoDetectParser(), source, sink, std::move(options), std::chrono::seconds(2));

    CHECK(sink.finished.load());
    CHECK_FALSE(sink.finishedCancelled.load());
    CHECK(sink.rowCount.load() == 2);
}
