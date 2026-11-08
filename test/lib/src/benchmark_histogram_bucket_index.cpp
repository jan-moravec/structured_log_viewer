// Micro-benchmarks for `loglib::HistogramBucketIndex`: rebuilding the index over 1 M rows has to take well under a
// frame. The figures are reported, not capped; CI machines vary, and the comparison script flags regressions.

#include "benchmark_common.hpp"
#include "benchmark_registry.hpp"

#include <loglib/core/log_level.hpp>
#include <loglib/core/log_value.hpp>
#include <loglib/query/histogram_bucket_index.hpp>

#include <benchmark/benchmark.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <vector>

using namespace loglib;

namespace bench
{

namespace
{

// Synthetic (ts, level) stream. Timestamps step by `step` from `BenchBase()`; levels come from a fixed info-heavy
// vocabulary.
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
    // Weighted like a production log: mostly info, some warn, occasional error/debug. Deterministic (seeded).
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

    // Fixed seed: reproducibility, not unpredictability.
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

constexpr std::size_t ROWS = 1'000'000;

struct RebuildInput
{
    std::vector<Event> events;
};

void RegisterRebuild(const char *name, HistogramBucketSize size, std::chrono::microseconds step)
{
    auto input = MakeLazy<RebuildInput>([step]() {
        auto created = std::make_unique<RebuildInput>();
        created->events = GenerateEvents(ROWS, step);
        return created;
    });
    RegisterSmall(name, [input, size](benchmark::State &state) {
        const RebuildInput &fixture = input->Get();
        std::size_t total = 0;
        for (auto _ : state)
        {
            HistogramBucketIndex index{size};
            for (const auto &ev : fixture.events)
            {
                index.AddRow(ev.ts, ev.level);
            }
            total = index.TotalRowCount();
            benchmark::DoNotOptimize(total);
        }
        if (total != ROWS)
        {
            Fail(state, "the index holds the wrong number of rows");
            return;
        }
        state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * ROWS));
    });
}

} // namespace

void RegisterHistogramBucketIndexBenchmarks()
{
    // 1 ms/row -> 1000 s span with a heavy fill per bucket (hot path).
    RegisterRebuild(
        "HistogramBucketIndex rebuild (1M rows / 1s buckets)",
        HistogramBucketSize::OneSecond,
        std::chrono::microseconds{1000}
    );
    // 60 ms/row -> 60000 s span.
    RegisterRebuild(
        "HistogramBucketIndex rebuild (1M rows / 1min buckets)",
        HistogramBucketSize::OneMinute,
        std::chrono::microseconds{60'000}
    );

    // Preload 900k events, then append 100k in small batches to simulate the 100 ms tail cadence.
    struct TailInput
    {
        std::vector<Event> preload;
        std::vector<Event> tail;
    };
    constexpr std::size_t PRELOAD = 900'000;
    constexpr std::size_t BATCH = 1'000;
    constexpr std::size_t BATCHES = 100;
    const auto tailStep = std::chrono::microseconds{1000};
    auto tailInput = MakeLazy<TailInput>([tailStep]() {
        auto created = std::make_unique<TailInput>();
        created->preload = GenerateEvents(PRELOAD, tailStep);
        created->tail = GenerateEvents(BATCH * BATCHES, tailStep);
        return created;
    });
    RegisterSmall(
        "HistogramBucketIndex live-tail (100x 1k row batches)", [tailInput, tailStep](benchmark::State &state) {
            const TailInput &fixture = tailInput->Get();
            std::size_t total = 0;
            for (auto _ : state)
            {
                HistogramBucketIndex index{HistogramBucketSize::OneSecond};
                for (const auto &ev : fixture.preload)
                {
                    index.AddRow(ev.ts, ev.level);
                }
                const auto tailStart = BenchBase() + tailStep * (PRELOAD + 1);
                for (std::size_t b = 0; b < BATCHES; ++b)
                {
                    for (std::size_t i = 0; i < BATCH; ++i)
                    {
                        const auto idx = (b * BATCH) + i;
                        index.AddRow(tailStart + tailStep * idx, fixture.tail[idx].level);
                    }
                }
                total = index.TotalRowCount();
                benchmark::DoNotOptimize(total);
            }
            if (total != PRELOAD + (BATCH * BATCHES))
            {
                Fail(state, "the index holds the wrong number of rows");
                return;
            }
            state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * total));
        }
    );

    // AutoBucketSize is O(1): a million calls take as long as a million additions of two numbers.
    RegisterSmall("HistogramBucketIndex::AutoBucketSize (1M calls)", [](benchmark::State &state) {
        const TimeStamp min = BenchBase();
        const TimeStamp max = BenchBase() + std::chrono::hours{24 * 365};
        HistogramBucketSize last = HistogramBucketSize::OneSecond;
        for (auto _ : state)
        {
            for (std::size_t i = 0; i < 1'000'000; ++i)
            {
                last = HistogramBucketIndex::AutoBucketSize(min, max);
                benchmark::DoNotOptimize(last);
            }
        }
        state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * 1'000'000));
    });
}

} // namespace bench
