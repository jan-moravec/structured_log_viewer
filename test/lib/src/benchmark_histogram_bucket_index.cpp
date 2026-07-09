// Micro-benchmarks for `loglib::HistogramBucketIndex`. Gates the
// ROADMAP-item-2 acceptance bar: rebuild the index over 1 M rows in
// well under a frame (target ~5 ms on the developer workstation the
// ROADMAP entry cites; CI machines vary, so the numbers are reported
// via `WARN` instead of a hard cap and the comparison script scrapes
// the log lines to flag regressions).

#include "benchmark_common.hpp"

#include <loglib/histogram_bucket_index.hpp>
#include <loglib/log_level.hpp>
#include <loglib/log_value.hpp>

#include <catch2/catch_all.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

using namespace loglib;

namespace
{

// A synthetic (ts, level) stream. Timestamps step by `stepUs` from
// `Base()`; levels cycle through a fixed vocabulary with the natural
// info-heavy distribution real logs have.
struct Event
{
    TimeStamp ts;
    LogLevel level;
};

TimeStamp BenchBase()
{
    constexpr auto EPOCH_SECONDS = std::chrono::seconds{1767225600}; // 2026-01-01T00:00:00Z
    return TimeStamp{std::chrono::duration_cast<std::chrono::microseconds>(EPOCH_SECONDS)};
}

std::vector<Event> GenerateEvents(std::size_t count, std::chrono::microseconds step)
{
    // Vocabulary weighted like a typical production log: mostly info,
    // some warn, occasional error/debug. Kept deterministic (seeded).
    static constexpr std::array<LogLevel, 10> LEVELS = {
        LogLevel::Info,
        LogLevel::Info,
        LogLevel::Info,
        LogLevel::Info,
        LogLevel::Info,
        LogLevel::Debug,
        LogLevel::Debug,
        LogLevel::Warn,
        LogLevel::Warn,
        LogLevel::Error,
    };

    // Deterministic fixture: reproducibility, not unpredictability, is the goal.
    // NOLINTNEXTLINE(cert-msc32-c,cert-msc51-cpp,bugprone-random-generator-seed)
    std::mt19937 rng{0xC0FFEEu};
    std::uniform_int_distribution<std::size_t> pick{0, LEVELS.size() - 1};

    std::vector<Event> events;
    events.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        events.push_back({.ts = BenchBase() + step * i, .level = LEVELS[pick(rng)]});
    }
    return events;
}

} // namespace

TEST_CASE("HistogramBucketIndex: rebuild 1M events / 1s buckets", "[.][benchmark][histogram]")
{
    BENCHMARK_REQUIRES_RELEASE_BUILD();

    constexpr std::size_t ROWS = 1'000'000;
    // Pick a step that gives us ~86400 rows/day at 1s buckets -> heavy
    // fill per bucket, which is the hot path.
    const auto step = std::chrono::microseconds{1000}; // 1ms/row -> 1000 s total span
    const auto events = GenerateEvents(ROWS, step);

    bench::RunTimedSamples(
        "HistogramBucketIndex rebuild (1M rows / 1s buckets)",
        5,
        [&]() {
            HistogramBucketIndex index{HistogramBucketSize::OneSecond};
            for (const auto &ev : events)
            {
                index.AddRow(ev.ts, ev.level);
            }
            REQUIRE(index.TotalRowCount() == ROWS);
        }
    );
}

TEST_CASE("HistogramBucketIndex: rebuild 1M events / 1min buckets", "[.][benchmark][histogram]")
{
    BENCHMARK_REQUIRES_RELEASE_BUILD();

    constexpr std::size_t ROWS = 1'000'000;
    const auto step = std::chrono::microseconds{60'000}; // 60ms/row -> 60000 s total span
    const auto events = GenerateEvents(ROWS, step);

    bench::RunTimedSamples(
        "HistogramBucketIndex rebuild (1M rows / 1min buckets)",
        5,
        [&]() {
            HistogramBucketIndex index{HistogramBucketSize::OneMinute};
            for (const auto &ev : events)
            {
                index.AddRow(ev.ts, ev.level);
            }
            REQUIRE(index.TotalRowCount() == ROWS);
        }
    );
}

TEST_CASE("HistogramBucketIndex: incremental live-tail append", "[.][benchmark][histogram]")
{
    BENCHMARK_REQUIRES_RELEASE_BUILD();

    // Preload the index with 900k events, then append 100k in small
    // batches to simulate the 100 ms tail cadence. Reports mean batch
    // append cost across 5 runs.
    constexpr std::size_t PRELOAD = 900'000;
    constexpr std::size_t BATCH = 1'000;
    constexpr std::size_t BATCHES = 100;
    const auto step = std::chrono::microseconds{1000};
    const auto preloadEvents = GenerateEvents(PRELOAD, step);
    const auto tailEvents = GenerateEvents(BATCH * BATCHES, step);

    bench::RunTimedSamples(
        "HistogramBucketIndex live-tail (100x 1k row batches)",
        5,
        [&]() {
            HistogramBucketIndex index{HistogramBucketSize::OneSecond};
            for (const auto &ev : preloadEvents)
            {
                index.AddRow(ev.ts, ev.level);
            }
            const auto tailStart = BenchBase() + step * (PRELOAD + 1);
            for (std::size_t b = 0; b < BATCHES; ++b)
            {
                for (std::size_t i = 0; i < BATCH; ++i)
                {
                    const auto idx = (b * BATCH) + i;
                    index.AddRow(tailStart + step * idx, tailEvents[idx].level);
                }
            }
            REQUIRE(index.TotalRowCount() == PRELOAD + (BATCH * BATCHES));
        }
    );
}

TEST_CASE("HistogramBucketIndex: AutoBucketSize is O(1)", "[.][benchmark][histogram]")
{
    BENCHMARK_REQUIRES_RELEASE_BUILD();

    const TimeStamp min = BenchBase();
    const TimeStamp max = BenchBase() + std::chrono::hours{24 * 365};

    bench::RunTimedSamples("HistogramBucketIndex::AutoBucketSize (1M calls)", 3, [&]() {
        volatile HistogramBucketSize sink = HistogramBucketSize::OneSecond;
        for (std::size_t i = 0; i < 1'000'000; ++i)
        {
            sink = HistogramBucketIndex::AutoBucketSize(min, max);
        }
        (void)sink;
    });
}
