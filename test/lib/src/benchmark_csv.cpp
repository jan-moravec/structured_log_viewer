// CSV parser benchmarks; mirror of the JSON and logfmt large and wide streaming benchmarks via the shared
// `benchmark_common.hpp`. Lines/s is comparable across formats; MB/s is not (per-cell footprints differ). See
// CONTRIBUTING.md `## Benchmarking`.

#include "benchmark_common.hpp"
#include "benchmark_registry.hpp"
#include "common.hpp"

#include <loglib/core/file_line_source.hpp>
#include <loglib/parse/log_parse_sink.hpp>
#include <loglib/parse/parser_options.hpp>
#include <loglib/parse/parsers/csv_parser.hpp>

#include <test_common/log_format.hpp>
#include <test_common/log_generator.hpp>

#include <cstddef>
#include <filesystem>
#include <random>
#include <utility>
#include <vector>

using namespace loglib;
using test_common::GenerateWideLogRecords;

namespace bench
{

namespace
{

// Free-function adapter (a `bench::ParserStreamFn` global would trip
// `bugprone-throwing-static-initialization` from `std::function`'s ctor).
void CsvStream(FileLineSource &source, LogParseSink &sink, const ParserOptions &options)
{
    CsvParser{}.ParseStreaming(source, sink, options);
}

// Derive the CSV schema by peeking one streamed record on a throwaway
// RNG seeded the same way as the real run (so the schema matches what
// the benchmark will emit, without materialising the full vector).
test_common::RecordSchema SampleStreamingSchema(std::uint32_t seed, const test_common::TimestampPolicy &timestamps)
{
    // NOLINTNEXTLINE(cert-msc32-c,cert-msc51-cpp,bugprone-random-generator-seed)
    std::mt19937 rng(seed);
    const test_common::LogRecord probe = test_common::GenerateRandomLogRecord(rng, 0, timestamps);
    return test_common::DeriveSchemaFromRecord(probe);
}

} // namespace

void RegisterCsvBenchmarks()
{
    // Large-file streaming benchmark, mirror of the JSON one. Records stream straight to disk -- no 1M-record vector
    // ever materialises.
    RegisterStreaming("Stream 1'000'000 CSV log entries to LogTable", []() {
        const test_common::TimestampPolicy timestamps = DeterministicBenchmarkTimestamps();
        const test_common::RecordSchema schema = SampleStreamingSchema(LARGE_FIXTURE_SEED, timestamps);
        return StreamingFixture{
            .logFile = std::make_unique<TestStructuredLogFile>(
                StreamedRecords{.count = 1'000'000, .seed = LARGE_FIXTURE_SEED, .timestamps = timestamps},
                test_common::Csv(schema),
                schema
            ),
            .parser = CsvStream,
        };
    });

    // Wide-row streaming benchmark. Field count matches the JSON one but per-field work differs (nested values land
    // as quoted compact-JSON cells), so cross-format lines/s is broadly -- not exactly -- comparable.
    RegisterStreaming("Stream 200'000 wide CSV log entries to LogTable", []() {
        auto records =
            GenerateWideLogRecords(200'000, /*columnCount=*/30, WIDE_FIXTURE_SEED, DeterministicBenchmarkTimestamps());
        const test_common::RecordSchema schema = test_common::DeriveSchemaFromRecord(records.front());
        return StreamingFixture{
            .logFile = std::make_unique<TestStructuredLogFile>(std::move(records), test_common::Csv(schema), schema),
            .parser = CsvStream,
        };
    });
}

} // namespace bench
