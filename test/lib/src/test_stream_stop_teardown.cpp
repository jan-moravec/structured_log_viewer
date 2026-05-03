// Stop-teardown coverage for `TailingBytesProducer` + `JsonParser::ParseStreaming`.
// The documented budget for stopping a live stream is 500 ms from
// `BytesProducer::Stop()` to the parser worker joining; on
// an idle source we expect *well* under that, while a worker mid-decode of a
// 100 KiB-buffered batch still has to honour the same budget.
//
// These tests deliberately drive the full source -> parser -> sink chain
// (not just the source on its own — `test_tailing_file_source.cpp` covers
// `TailingBytesProducer::Stop` in isolation) so any regression in the parser
// stop-token plumbing surfaces here.

#include <loglib/parsers/json_parser.hpp>
#include <loglib/key_index.hpp>
#include <loglib/parser_options.hpp>
#include <loglib/stop_token.hpp>
#include <loglib/stream_line_source.hpp>
#include <loglib/log_parse_sink.hpp>
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
using loglib::ParserOptions;
using loglib::StopSource;
using loglib::StreamedBatch;
using loglib::LogParseSink;
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

/// Test sink that counts incoming batches and reports `OnFinished` so the
/// test can assert the parser exited cleanly. We can't reuse the GUI's
/// `QtStreamingLogSink` here because the library tests don't link Qt.
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

//  success metric 6: a worker parked in `WaitForBytes` on an idle
// producer must observe `BytesProducer::Stop()` and unwind in <= 500 ms. This
// is the typical case (the parser flushes between reads and parks until
// new bytes / the deadline / Stop). The end-to-end timed window covers
// `BytesProducer::Stop()` through `ParserOptions::stopToken.request_stop()`,
// `joinable thread join`, and `OnFinished`.
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
    // `LogModel::BeginStreaming`'s `QtConcurrent::run` path without the Qt
    // dependency). The worker's hot loop polls the parser stop-token at
    // every batch boundary and on every `WaitForBytes` wake.
    JsonParser parser;
    std::thread worker([&] { parser.ParseStreaming(*streamPtr, sink, std::move(options)); });

    // Give the worker time to drain pre-fill and park. The test's correctness
    // doesn't depend on this sleep — Stop() is observed even if the worker
    // is mid-Read — but the typical-case wall-clock we're measuring is
    // dominated by `WaitForBytes` parking, so we aim for the worker to be
    // there before we Stop.
    std::this_thread::sleep_for(ScaledMs(100ms));

    const auto stopStart = std::chrono::steady_clock::now();
    producerPtr->Stop();
    stopSource.request_stop();
    worker.join();
    const auto elapsed = std::chrono::steady_clock::now() - stopStart;

    CHECK(sink.finished.load(std::memory_order_acquire));
    INFO("Stop teardown elapsed: " << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() << " ms");
    //  success metric 6: ≤ 500 ms. We allow `LOGLIB_TEST_TIME_SCALE`
    // to widen the budget on slow CI runners — the  is a
    // developer-class-machine target, not a hard real-time bound.
    CHECK(elapsed < ScaledMs(500ms));
}

//  success metric 6 (2nd half): a worker mid-decoding a 100 KiB
// buffered batch must still observe Stop within the same budget. We
// pre-write a 100 KiB JSONL block before the parser starts, so the
// parser's hot loop is busy decoding (rather than parked in
// `WaitForBytes`) when Stop is signalled. The parser checks the
// stop-token at every line boundary, so the worst-case decode latency
// is one line-decode cycle plus the Stop-then-join wait.
TEST_CASE("Stream Stop teardown stops a mid-decode worker within 500 ms", "[stream_stop_teardown]")
{
    TempDir dir;
    const auto path = dir.File("buffered.log");

    // ~100 KiB buffered batch: 1500 lines x ~70 bytes each.
    constexpr int kLines = 1500;
    {
        std::string blob;
        blob.reserve(100 * 1024);
        for (int i = 0; i < kLines; ++i)
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

    // Don't wait — Stop the worker as soon as possible so we hit it
    // mid-decode rather than after natural drain. A short sleep ensures
    // OnStarted has fired, otherwise the test could measure pre-stream
    // setup time as part of the budget.
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
