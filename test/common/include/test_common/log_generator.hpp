#pragma once

#include <test_common/log_record.hpp>

#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

namespace test_common
{

// Deterministic seed entry-point for reproducible fixtures.
std::uint32_t MakeRandomSeed();

// `count` random log records (timestamp/level/message/component shape).
std::vector<LogRecord> GenerateRandomLogRecords(std::size_t count);
std::vector<LogRecord> GenerateRandomLogRecords(std::size_t count, std::uint32_t seed);

// Wide-row variant (many columns of mixed value families).
std::vector<LogRecord> GenerateWideLogRecords(std::size_t count, std::size_t columnCount = 30);
std::vector<LogRecord> GenerateWideLogRecords(std::size_t count, std::size_t columnCount, std::uint32_t seed);

// Single-record variant for streaming producers (the standalone
// `log_generator` and the streaming benchmark fixture), so they can write
// incrementally without buffering the whole sequence. Output of
// `GenerateRandomLogRecord(rng, i)` in a loop matches `GenerateRandomLogRecords`
// (modulo RNG state).
LogRecord GenerateRandomLogRecord(std::mt19937 &rng, std::size_t lineIndex);

} // namespace test_common
