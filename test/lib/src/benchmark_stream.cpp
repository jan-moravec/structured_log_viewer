// Stream-Mode end-to-end latency benchmark.
//
// Measures the wall-clock delta between a producer's write()+fflush() and the corresponding row landing in
// OnBatch, on the parser-side path (no Qt event loop). The median latency of a run is the figure; the 95th
// percentile and the maximum are reported as counters.

#include "benchmark_common.hpp"
#include "benchmark_registry.hpp"
#include "common.hpp"

#include <loglib/core/key_index.hpp>
#include <loglib/core/log_line.hpp>
#include <loglib/core/stop_token.hpp>
#include <loglib/core/stream_line_source.hpp>
#include <loglib/ingest/tailing_bytes_producer.hpp>
#include <loglib/parse/log_parse_sink.hpp>
#include <loglib/parse/parser_options.hpp>
#include <loglib/parse/parsers/json_parser.hpp>

#include <benchmark/benchmark.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using loglib::JsonParser;
using loglib::KeyIndex;
using loglib::LogParseSink;
using loglib::ParserOptions;
using loglib::RowBatch;
using loglib::StopSource;
using loglib::StreamLineSource;
using loglib::TailingBytesProducer;
using namespace std::chrono_literals;

namespace bench
{

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
        mPath = base / ("loglib_stream_latency_" + std::to_string(suffix));
        std::filesystem::create_directories(mPath);
    }
    // MSVC may model throwing paths through STL remove_all; teardown
    // ignores errors via error_code.
    // NOLINTNEXTLINE(bugprone-exception-escape)
    ~TempDir() noexcept
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

/**
 * Sink that timestamps every line as it arrives, paired with a side-channel
 * vector of producer-side write()-completion timestamps so the benchmark
 * driver can compute the per-line delta. lineId is the canonical match
 * key (the parser stamps it on the emitted LogLine via
 * StreamLineSource::AppendLine).
 */
struct LatencyMeasuringSink final : LogParseSink
{
    KeyIndex keys;
    std::mutex mu;
    /**
     * (lineId, OnBatch arrival timestamp) for every line the parser
     * emitted. Populated under mu so the benchmark thread can read it
     * after Stop without racing the worker.
     */
    std::vector<std::pair<size_t, std::chrono::steady_clock::time_point>> arrivals;
    std::atomic<bool> finished{false};

    KeyIndex &Keys() override
    {
        return keys;
    }
    void OnStarted() override
    {
    }
    void OnBatch(RowBatch batch) override
    {
        const auto now = std::chrono::steady_clock::now();
        const std::scoped_lock lock(mu);
        arrivals.reserve(arrivals.size() + batch.lines.size());
        for (const auto &line : batch.lines)
        {
            arrivals.emplace_back(line.LineId(), now);
        }
    }
    void OnFinished(bool /*cancelled*/) override
    {
        finished.store(true, std::memory_order_release);
    }
};

/**
 * The producer thread writes one JSON line at a time to a pre-opened
 * FILE*, calling fflush after each line so the consumer sees a
 * complete \n-terminated line every iteration. We record the
 * post-fflush timestamp as the canonical "byte hit disk" moment so the
 * downstream latency includes the producer's syscall path too (it does
 * not include the test harness's own scheduling jitter on the producer
 * side, but that is sub-millisecond on every platform we care about).
 */
struct ProducerWrite
{
    size_t lineId;
    std::chrono::steady_clock::time_point committedAt;
};

void RunProducer(
    FILE *fp,
    int lineCount,
    std::chrono::microseconds interLineDelay,
    std::vector<ProducerWrite> &writes,
    std::atomic<bool> &producerDone
)
{
    writes.reserve(static_cast<size_t>(lineCount));
    for (int i = 0; i < lineCount; ++i)
    {
        const auto lineId = static_cast<size_t>(static_cast<size_t>(i) + 1u);
        // Stamp the line with its own id so the consumer can match
        // LineId -> arrival timestamp without per-line bookkeeping in the
        // sink. Padding keeps the line size representative of real log data.
        char buf[160];
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): benchmark I/O; fixed buffer + bounded format.
        const int n = std::snprintf(
            buf, sizeof(buf), "{\"i\":%zu,\"msg\":\"latency benchmark line %zu padding padding\"}\n", lineId, lineId
        );
        if (n <= 0)
        {
            break;
        }
        static_cast<void>(std::fwrite(buf, 1, static_cast<size_t>(n), fp));
        static_cast<void>(std::fflush(fp));
        const auto committedAt = std::chrono::steady_clock::now();
        writes.push_back({.lineId = lineId, .committedAt = committedAt});

        if (interLineDelay.count() > 0)
        {
            std::this_thread::sleep_for(interLineDelay);
        }
    }
    producerDone.store(true, std::memory_order_release);
}

double Percentile(std::vector<double> sorted, double pct)
{
    if (sorted.empty())
    {
        return 0.0;
    }
    std::ranges::sort(sorted);
    const auto idx = static_cast<size_t>(std::lround((pct / 100.0) * static_cast<double>(sorted.size() - 1)));
    return sorted[std::min(idx, sorted.size() - 1)];
}

// One run: the producer writes LINE_COUNT lines at a steady rate well below saturation while the parser drains in real
// time. Returns the per-line write-to-row deltas in milliseconds, empty when the run failed. The full GUI latency is
// verified separately by the offscreen Qt smoke test in test/app/src/main_window_test.cpp.
//
// 100 lines at 10 ms give the worker plenty of poll-tick cycles to coalesce several lines per batch, so the
// measurement stresses the coalesce window and not only the syscall round-trip, and a run takes about a second.
std::vector<double> RunLatencyOnce()
{
    constexpr int LINE_COUNT = 100;
    constexpr auto INTER_LINE_DELAY = 10ms;

    const TempDir dir;
    const auto path = dir.File("latency.log");
    {
        const std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
        {
            return {};
        }
    }

    TailingBytesProducer::Options sourceOptions;
    sourceOptions.disableNativeWatcher = false;
    sourceOptions.pollInterval = 25ms;
    sourceOptions.rotationDebounce = 1000ms;
    sourceOptions.readChunkBytes = 64 * 1024;
    sourceOptions.prefillChunkBytes = 64 * 1024;

    auto tailProducer = std::make_unique<TailingBytesProducer>(path, /*retentionLines=*/LINE_COUNT * 2, sourceOptions);
    TailingBytesProducer *producerPtr = tailProducer.get();
    auto streamSource = std::make_unique<StreamLineSource>(path, std::move(tailProducer));
    StreamLineSource *streamPtr = streamSource.get();

    LatencyMeasuringSink sink;
    StopSource stopSource;
    ParserOptions options;
    options.stopToken = stopSource.GetToken();

    JsonParser parser;
    std::thread worker([&] { parser.ParseStreaming(*streamPtr, sink, std::move(options)); });

    // Open the file with the C stdio API so we can fflush per line; the C++ std::ofstream flush is buffered by the
    // streambuf even after flush() calls in some implementations. fflush(fp) issues the _write syscall
    // directly. MSVC prefers fopen_s; the safer API adds nothing here (the path is test-controlled) so suppress
    // C4996 locally rather than fork the call site by platform.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    FILE *fp = std::fopen(path.string().c_str(), "ab");
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    if (fp == nullptr)
    {
        producerPtr->Stop();
        stopSource.RequestStop();
        worker.join();
        return {};
    }

    std::vector<ProducerWrite> writes;
    std::atomic<bool> producerDone{false};
    std::thread producer([&] {
        RunProducer(
            fp,
            LINE_COUNT,
            std::chrono::duration_cast<std::chrono::microseconds>(INTER_LINE_DELAY),
            writes,
            producerDone
        );
    });

    producer.join();
    static_cast<void>(std::fclose(fp));

    // Wait for the consumer to drain. The parser keeps parking on WaitForBytes because nothing more is coming, so we
    // Stop after a grace window. The window must exceed the worst-case latency we are willing to measure, so a stuck
    // line at the tail surfaces as an outlier in the percentiles rather than a missed sample.
    const auto deadline = std::chrono::steady_clock::now() + 1500ms;
    while (std::chrono::steady_clock::now() < deadline)
    {
        {
            const std::scoped_lock lock(sink.mu);
            if (static_cast<int>(sink.arrivals.size()) >= LINE_COUNT)
            {
                break;
            }
        }
        std::this_thread::sleep_for(5ms);
    }

    producerPtr->Stop();
    stopSource.RequestStop();
    worker.join();

    if (!sink.finished.load(std::memory_order_acquire))
    {
        return {};
    }

    // Match write timestamps against arrivals via lineId; compute deltas.
    std::vector<double> latenciesMs;
    latenciesMs.reserve(static_cast<size_t>(LINE_COUNT));
    const std::scoped_lock lock(sink.mu);
    // The parser emits in source order, so the arrivals are normally sorted by lineId already, but we do not rely on
    // it.
    std::ranges::sort(sink.arrivals, [](const auto &a, const auto &b) { return a.first < b.first; });
    size_t arrIdx = 0;
    for (const auto &write : writes)
    {
        while (arrIdx < sink.arrivals.size() && sink.arrivals[arrIdx].first < write.lineId)
        {
            ++arrIdx;
        }
        if (arrIdx >= sink.arrivals.size() || sink.arrivals[arrIdx].first != write.lineId)
        {
            continue; // dropped on Stop -- skip rather than fail the run
        }
        const auto delta = sink.arrivals[arrIdx].second - write.committedAt;
        latenciesMs.push_back(std::chrono::duration<double, std::milli>(delta).count());
        ++arrIdx;
    }
    return latenciesMs;
}

struct NoInput
{
};

} // namespace

void RegisterStreamBenchmarks()
{
    // The iteration time is the median write-to-row latency of one run. The figures are reported and compared between
    // runs, never limited.
    RegisterLarge<NoInput>(
        "Stream Mode write-to-row latency",
        [] { return std::make_unique<NoInput>(); },
        [](NoInput & /*input*/, benchmark::State &state, bool first) {
            if (first)
            {
                RunLatencyOnce();
            }
            double p95 = 0.0;
            double maxLatency = 0.0;
            std::size_t measured = 0;
            for (const auto _ : state)
            {
                std::vector<double> latenciesMs = RunLatencyOnce();
                if (latenciesMs.empty())
                {
                    Fail(state, "no line was measured");
                    return;
                }
                std::ranges::sort(latenciesMs);
                measured = latenciesMs.size();
                p95 = Percentile(latenciesMs, 95.0);
                maxLatency = latenciesMs.back();
                state.SetIterationTime(Percentile(latenciesMs, 50.0) / 1000.0);
            }
            state.counters["lines_measured"] = static_cast<double>(measured);
            state.counters["p95_ms"] = p95;
            state.counters["max_ms"] = maxLatency;
        },
        LargeOptions{.manualTime = true}
    );
}

} // namespace bench
