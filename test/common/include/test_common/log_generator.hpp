#pragma once

#include <test_common/json_log_line.hpp>

#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

namespace test_common
{

// Deterministic seed entry-point for reproducible fixtures.
std::uint32_t MakeRandomSeed();

// `count` random JSON log lines (timestamp/level/message/component shape).
std::vector<JsonLogLine> GenerateRandomJsonLogs(std::size_t count);
std::vector<JsonLogLine> GenerateRandomJsonLogs(std::size_t count, std::uint32_t seed);

// Wide-row variant.
std::vector<JsonLogLine> GenerateWideJsonLogs(std::size_t count, std::size_t columnCount = 30);
std::vector<JsonLogLine> GenerateWideJsonLogs(std::size_t count, std::size_t columnCount, std::uint32_t seed);

// Single-line variant for the streaming `log_generator`, so it can write
// incrementally without buffering the whole file. Output of
// `GenerateRandomJsonLogLine(rng, i)` in a loop matches `GenerateRandomJsonLogs`
// (modulo RNG state).
JsonLogLine GenerateRandomJsonLogLine(std::mt19937 &rng, std::size_t lineIndex);

} // namespace test_common
