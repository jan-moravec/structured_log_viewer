/**
 * @brief The seeds and timestamps that make the generated benchmark and memory fixtures identical on every run.
 *
 * The benchmarks and the memory-per-row test build their log files from these, so they all describe the same rows. The
 * constants are separate from the benchmark helpers so that a unit test can use them without Google Benchmark.
 */
#pragma once

#include <test_common/log_generator.hpp>

#include <chrono>
#include <cstdint>

namespace bench
{

/**
 * @brief The seed of the large fixture that the JSON, logfmt and CSV benchmarks share.
 *
 * With DeterministicBenchmarkTimestamps it makes every format carry the same records, so lines per second compare
 * across formats. Always use the two together: without fixed timestamps each record takes the current time and the file
 * changes by a few bytes on every run.
 */
inline constexpr std::uint32_t LARGE_FIXTURE_SEED = 0xC0FFEEu;

/**
 * @brief The seed of the wide-row fixture. It differs from LARGE_FIXTURE_SEED so the two shapes never share records.
 * Use it with DeterministicBenchmarkTimestamps too.
 */
inline constexpr std::uint32_t WIDE_FIXTURE_SEED = 0xDEC0DEu;

/**
 * @brief Fixed timestamps for the fixtures: 2026-01-01 00:00:00 UTC, then 1 ms per record. The step keeps every
 * formatted timestamp the same length.
 */
inline test_common::TimestampPolicy DeterministicBenchmarkTimestamps()
{
    // 2026-01-01T00:00:00Z = 1767225600 seconds since the Unix epoch.
    constexpr std::chrono::seconds BENCHMARK_BASE_TIME_EPOCH{1767225600};
    return {
        .baseTime = std::chrono::system_clock::time_point{BENCHMARK_BASE_TIME_EPOCH},
        .interval = std::chrono::milliseconds{1},
    };
}

} // namespace bench
