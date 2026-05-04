// Stop-teardown coverage for `TailingBytesProducer` +
// `JsonParser::ParseStreaming`. Budget: 500 ms from
// `BytesProducer::Stop()` to the parser worker joining, both for an
// idle source and for a worker mid-decode of a 100 KiB buffered batch.
//
// These tests drive the full source -> parser -> sink chain so any
// regression in the parser stop-token plumbing surfaces here.

#include <loglib/key_index.hpp>
#include <loglib/log_parse_sink.hpp>
#include <loglib/parser_options.hpp>
#include <loglib/parsers/json_parser.hpp>
#include <loglib/stop_token.hpp>
#include <loglib/stream_line_source.hpp>
#include <loglib/tailing_bytes_producer.hpp>

#include <memory>

#include <loglib_test/scaled_ms.hpp>

#include <catch2/catch_all.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <utility>

using loglib::JsonParser;
using loglib::KeyIndex;
using loglib::LogParseSink;
using loglib::ParserOptions;
using loglib::StopSource;
using loglib::StreamedBatch;
using loglib::StreamLineSource;
using loglib::TailingBytesProducer;
using loglib_test::ScaledMs;
using namespace std::chrono_literals;

namespace
{

class TempDir
{
public:
    TempDir()
    {
        auto base = std::filesystem::temp_directory_path();
        std::random_device rd;
        std::mt19937_64 gen(rd());
        const uint64_t suffix = gen();
        mPath = base / ("loglib_stop_teardown_" + std::to_string(suffix));
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

void Append(const std::filesystem::path &path, std::string_view text)
{
    std::ofstream out(path, std::ios::binary | std::ios::app);
    REQUIRE(out.is_open());
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.flush();
}

/// Counts batches and reports `OnFinished` so we can assert clean exit.
/// (Library tests don't link Qt, so `QtStreamingLogSink` isn't available.)
struct CountingSink final : LogParseSink
{
    KeyIndex keys;
    std::atomic<size_t> batchCount{0};
    std::atomic<size_t> rowCount{0};
    std::atomic<bool> finished{false};
    std::atomic<bool> finishedCancelled{false};

    KeyIndex &Keys() override
    {
        return keys;
    }
    void OnStarted() override
    {
    }
    void OnBatch(StreamedBatch batch) override
    {
        rowCount.fetch_add(batch.lines.size(), std::memory_order_relaxed);
        batchCount.fetch_add(1, std::memory_order_relaxed);
    }
    void OnFinished(bool cancelled) override
    {
        finishedCancelled.store(cancelled, std::memory_order_release);
        finished.store(true, std::memory_order_release);
    }
};

TailingBytesProducer::Options TestOptions()
{
    TailingBytesProducer::Options options;
    options.disableNativeWatcher = true;
    options.pollInterval = ScaledMs(25ms);
    options.rotationDebounce = ScaledMs(250ms);
    options.readChunkBytes = 64 * 1024;
    options.prefillChunkBytes = 64 * 1024;
    return options;
}

} // namespace

// A worker parked in `WaitForBytes` on an idle producer must observe
// `BytesProducer::Stop()` and unwind within 500 ms. The timed window
// covers Stop() -> request_stop() -> thread join -> OnFinished.
TEST_CASE("Stream Stop teardown unblocks a worker parked in WaitForBytes within 500 ms", "[stream_stop_teardown]")
{
    TempDir dir;
    const auto path = dir.File("idle.log");
    // Start with one line so the parser drains pre-fill, then parks.
    Append(path, "{\"a\":1}\n");

    auto producer = std::make_unique<TailingBytesProducer>(path, /*retentionLines=*/100, TestOptions());
    TailingBytesProducer *producerPtr = producer.get();
    auto streamSource = std::make_unique<StreamLineSource>(path, std::move(producer));
    StreamLineSource *streamPtr = streamSource.get();

    CountingSink sink;
    StopSource stopSource;
    ParserOptions options;
    options.stopToken = stopSource.get_token();

    // Drive the parser worker on a dedicated thread (mirrors
    // `LogModel::BeginStreaming`'s `QtConcurrent::run` without Qt).
    JsonParser parser;
    std::thread worker([&] { parser.ParseStreaming(*streamPtr, sink, std::move(options)); });

    // Give the worker time to drain pre-fill and park in WaitForBytes
    // so we measure the typical idle path (Stop is correct either way).
    std::this_thread::sleep_for(ScaledMs(100ms));

    const auto stopStart = std::chrono::steady_clock::now();
    producerPtr->Stop();
    stopSource.request_stop();
    worker.join();
    const auto elapsed = std::chrono::steady_clock::now() - stopStart;

    CHECK(sink.finished.load(std::memory_order_acquire));
    INFO("Stop teardown elapsed: " << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() << " ms");
    // 500 ms developer-class-machine budget. `LOGLIB_TEST_TIME_SCALE`
    // widens it for slow CI runners.
    CHECK(elapsed < ScaledMs(500ms));
}

// A worker mid-decoding a 100 KiB buffered batch must still observe
// Stop within 500 ms. The parser checks the stop-token at every line
// boundary, so worst case is one line decode plus Stop-then-join.
TEST_CASE("Stream Stop teardown stops a mid-decode worker within 500 ms", "[stream_stop_teardown]")
{
    TempDir dir;
    const auto path = dir.File("buffered.log");

    // ~100 KiB buffered batch: 1500 lines x ~70 bytes each.
    constexpr int LINE_COUNT = 1500;
    {
        std::string blob;
        blob.reserve(100 * 1024);
        for (int i = 0; i < LINE_COUNT; ++i)
        {
            blob += "{\"i\":" + std::to_string(i) + ",\"msg\":\"line ";
            blob += std::to_string(i);
            blob += " padding padding padding\"}\n";
        }
        Append(path, blob);
    }

    auto producer = std::make_unique<TailingBytesProducer>(path, /*retentionLines=*/2000, TestOptions());
    TailingBytesProducer *producerPtr = producer.get();
    auto streamSource = std::make_unique<StreamLineSource>(path, std::move(producer));
    StreamLineSource *streamPtr = streamSource.get();

    CountingSink sink;
    StopSource stopSource;
    ParserOptions options;
    options.stopToken = stopSource.get_token();

    JsonParser parser;
    std::thread worker([&] { parser.ParseStreaming(*streamPtr, sink, std::move(options)); });

    // Stop ASAP so we hit the worker mid-decode. The brief sleep just
    // ensures OnStarted has fired so we don't measure setup time.
    std::this_thread::sleep_for(ScaledMs(5ms));

    const auto stopStart = std::chrono::steady_clock::now();
    producerPtr->Stop();
    stopSource.request_stop();
    worker.join();
    const auto elapsed = std::chrono::steady_clock::now() - stopStart;

    CHECK(sink.finished.load(std::memory_order_acquire));
    INFO(
        "Mid-decode Stop teardown elapsed: " << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
                                             << " ms; consumed batches=" << sink.batchCount.load()
                                             << " rows=" << sink.rowCount.load()
    );
    CHECK(elapsed < ScaledMs(500ms));
}
