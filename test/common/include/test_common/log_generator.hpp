#pragma once

#include <test_common/log_record.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <vector>

namespace test_common
{

// Deterministic seed entry-point for reproducible fixtures.
std::uint32_t MakeRandomSeed();

// Controls how `timestamp` is stamped on each generated record.
//
// - Default (`baseTime == nullopt`): wall-clock reading per record. Used
//   by the standalone `log_generator` so consumers see realistic times.
// - `baseTime` set: deterministic `baseTime + lineIndex * interval`.
//   Benchmarks use this so a pinned seed yields byte-identical records
//   across runs and across formats.
struct TimestampPolicy
{
    std::optional<std::chrono::system_clock::time_point> baseTime;
    std::chrono::milliseconds interval{1};
};

// `count` random records (timestamp/level/message/component shape).
std::vector<LogRecord> GenerateRandomLogRecords(std::size_t count);
std::vector<LogRecord> GenerateRandomLogRecords(
    std::size_t count, std::uint32_t seed, const TimestampPolicy &timestamps = {}
);

// Wide-row variant (many columns of mixed value families).
std::vector<LogRecord> GenerateWideLogRecords(std::size_t count, std::size_t columnCount = 30);
std::vector<LogRecord> GenerateWideLogRecords(
    std::size_t count, std::size_t columnCount, std::uint32_t seed, const TimestampPolicy &timestamps = {}
);

// Single-record variant for streaming producers. Calling this in a loop
// matches `GenerateRandomLogRecords(count, seed, policy)` modulo RNG state.
LogRecord GenerateRandomLogRecord(std::mt19937 &rng, std::size_t lineIndex, const TimestampPolicy &timestamps = {});

} // namespace test_common
