// CSV parser benchmarks; mirror of `[json_parser]` / `[logfmt_parser]`
// `[large]` and `[wide]` via the shared `benchmark_common.hpp`. Lines/s
// is comparable across formats; MB/s is not (per-cell footprints
// differ). See CONTRIBUTING.md `## Benchmarking`.

#include "benchmark_common.hpp"
#include "common.hpp"

#include <loglib/file_line_source.hpp>
#include <loglib/internal/advanced_parser_options.hpp>
#include <loglib/log_parse_sink.hpp>
#include <loglib/parser_options.hpp>
#include <loglib/parsers/csv_parser.hpp>

#include <test_common/log_format.hpp>
#include <test_common/log_generator.hpp>

#include <catch2/catch_all.hpp>

#include <cstddef>
#include <filesystem>
#include <random>
#include <utility>
#include <vector>

using namespace loglib;
using namespace bench;
using test_common::GenerateWideLogRecords;

namespace
{

// Free-function adapter (a `bench::ParserStreamFn` global would trip
// `bugprone-throwing-static-initialization` from `std::function`'s ctor).
void CsvStream(
    FileLineSource &source, LogParseSink &sink, const ParserOptions &options, internal::AdvancedParserOptions advanced
)
{
    CsvParser::ParseStreaming(source, sink, options, advanced);
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

// Large-file streaming benchmark, mirror of `[json_parser]` /
// `[logfmt_parser]` `[large]`. Records stream straight to disk -- no
// 1M-record vector ever materialises.
TEST_CASE("Stream CSV log to LogTable (1'000'000 lines)", "[.][benchmark][csv_parser][large]")
{
    BENCHMARK_REQUIRES_RELEASE_BUILD();

    const test_common::TimestampPolicy timestamps = DeterministicBenchmarkTimestamps();
    const test_common::RecordSchema schema = SampleStreamingSchema(LARGE_FIXTURE_SEED, timestamps);

    const TestStructuredLogFile testFile(
        StreamedRecords{.count = 1'000'000, .seed = LARGE_FIXTURE_SEED, .timestamps = timestamps},
        test_common::Csv(schema),
        schema
    );
    const size_t bytes = std::filesystem::file_size(testFile.GetFilePath());

    InitializeTimezoneData();

    auto configuration = MakeTimestampConfiguration();
    const TestLogConfiguration configFile;
    configFile.Write(*configuration);

    RunStreamingBenchmark(
        "Stream 1'000'000 CSV log entries to LogTable",
        configFile.GetFilePath(),
        testFile.GetFilePath(),
        configuration,
        CsvStream,
        testFile.RecordCount(),
        bytes,
        4
    );
}

// Wide-row streaming benchmark, mirror of `[json_parser][wide]` /
// `[logfmt_parser][wide]`. Field count matches but per-field work
// differs (nested values land as quoted compact-JSON cells), so
// cross-format lines/s is broadly -- not exactly -- comparable.
TEST_CASE("Stream CSV log to LogTable (wide, 200'000 lines)", "[.][benchmark][csv_parser][wide]")
{
    BENCHMARK_REQUIRES_RELEASE_BUILD();

    auto records =
        GenerateWideLogRecords(200'000, /*columnCount=*/30, WIDE_FIXTURE_SEED, DeterministicBenchmarkTimestamps());
    const test_common::RecordSchema schema = test_common::DeriveSchemaFromRecord(records.front());

    const TestStructuredLogFile testFile(std::move(records), test_common::Csv(schema), schema);
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    const size_t bytes = std::filesystem::file_size(testFile.GetFilePath());

    InitializeTimezoneData();

    auto configuration = MakeTimestampConfiguration();
    const TestLogConfiguration configFile;
    configFile.Write(*configuration);

    RunStreamingBenchmark(
        "Stream 200'000 wide CSV log entries to LogTable",
        configFile.GetFilePath(),
        testFile.GetFilePath(),
        configuration,
        CsvStream,
        testFile.RecordCount(),
        bytes,
        4
    );
}
