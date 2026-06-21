// CSV parser benchmarks. Mirror of `[json_parser]` / `[logfmt_parser]`
// `[large]` and `[wide]` cases through the shared `benchmark_common.hpp`
// harness; lines/s is directly comparable across formats (MB/s differs
// because CSV's per-cell footprint differs from JSON / logfmt). See
// CONTRIBUTING.md `## Benchmarking`. Debug builds skip via
// `BENCHMARK_REQUIRES_RELEASE_BUILD`.

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

// `CsvParser::ParseStreaming` adapter for `bench::RunStreamingBenchmark`.
// Free function (rather than a `bench::ParserStreamFn` global) to avoid
// `bugprone-throwing-static-initialization` from `std::function`'s ctor.
void CsvStream(
    FileLineSource &source, LogParseSink &sink, const ParserOptions &options, internal::AdvancedParserOptions advanced
)
{
    CsvParser::ParseStreaming(source, sink, options, advanced);
}

// Peek the first record the streaming generator would emit so we can
// derive the CSV column schema without materialising the full record
// vector. The streaming benchmark below then runs against the same
// `seed + timestamps`, producing identical record bytes; the probe
// just consumes one draw of a throwaway RNG.
test_common::RecordSchema SampleStreamingSchema(
    std::uint32_t seed, const test_common::TimestampPolicy &timestamps
)
{
    // NOLINTNEXTLINE(cert-msc32-c,cert-msc51-cpp,bugprone-random-generator-seed)
    std::mt19937 rng(seed);
    test_common::LogRecord probe = test_common::GenerateRandomLogRecord(rng, 0, timestamps);
    return test_common::DeriveSchemaFromRecord(probe);
}

} // namespace

// Large-file streaming benchmark (1'000'000 lines). Mirrors the JSON /
// logfmt `[large]` cases. Records are produced by the streaming ctor so
// the 1M records never materialise as a vector; only the bytes hit disk.
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
// `[logfmt_parser][wide]`. Stresses the per-cell hot loop in
// `CsvLineDecoder`. Nested array/object fields land as quoted
// compact-JSON cells (see `Csv()`), so the field count matches
// `[json_parser][wide]` but per-field work differs -- treat cross-format
// lines/s as broadly comparable, not exactly.
TEST_CASE("Stream CSV log to LogTable (wide, 200'000 lines)", "[.][benchmark][csv_parser][wide]")
{
    BENCHMARK_REQUIRES_RELEASE_BUILD();

    auto records =
        GenerateWideLogRecords(200'000, /*columnCount=*/30, WIDE_FIXTURE_SEED, DeterministicBenchmarkTimestamps());
    // Derive the schema from the first record (lex-ordered keys; every
    // record in the wide vector shares the same key set).
    test_common::RecordSchema schema = test_common::DeriveSchemaFromRecord(records.front());

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
