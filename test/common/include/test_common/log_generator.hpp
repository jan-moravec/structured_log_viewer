#pragma once

#include <test_common/json_log_line.hpp>

#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

namespace test_common
{

// Deterministic seed entry-point. Prefer this over the seedless overloads
// when reproducibility matters (e.g. fixture replay in CI).
std::uint32_t MakeRandomSeed();

// Generate `count` random JSON log lines that look like a typical
// timestamp/level/message/component record. Equivalent to the local
// `GenerateRandomJsonLogs` helper that previously lived in
// `test/lib/src/benchmark_json.cpp` - moved here so the standalone
// `log_generator` console app can reuse it without pulling Catch2.
std::vector<JsonLogLine> GenerateRandomJsonLogs(std::size_t count);
std::vector<JsonLogLine> GenerateRandomJsonLogs(std::size_t count, std::uint32_t seed);

// Wide-row variant. See the long comment in the original
// `GenerateWideJsonLogs` definition for the field-mix rationale.
std::vector<JsonLogLine> GenerateWideJsonLogs(std::size_t count, std::size_t columnCount = 30);
std::vector<JsonLogLine> GenerateWideJsonLogs(std::size_t count, std::size_t columnCount, std::uint32_t seed);

// Single-line variant the streaming `log_generator` calls in a loop so the
// app can write incrementally without buffering the whole file in RAM.
// `lineIndex` is folded into the synthetic `thread_id` field the same way
// the batch generator does, so a stream of `GenerateRandomJsonLogLine(rng,
// i)` calls produces output equivalent to `GenerateRandomJsonLogs(count)`
// (modulo the per-call RNG state).
JsonLogLine GenerateRandomJsonLogLine(std::mt19937 &rng, std::size_t lineIndex);

} // namespace test_common
