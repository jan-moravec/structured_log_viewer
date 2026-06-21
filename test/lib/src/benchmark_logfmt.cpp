// logfmt parser benchmarks. Mirror of the JSON `[large]` and `[wide]`
// cases through the shared `benchmark_common.hpp` harness; lines/s is
// directly comparable across formats (MB/s differs because logfmt has
// fewer bytes/line than JSON). See CONTRIBUTING.md `## Benchmarking`.
// Debug builds skip via `BENCHMARK_REQUIRES_RELEASE_BUILD`.

#include "benchmark_common.hpp"
#include "common.hpp"

#include <loglib/file_line_source.hpp>
#include <loglib/internal/advanced_parser_options.hpp>
#include <loglib/log_parse_sink.hpp>
#include <loglib/parser_options.hpp>
#include <loglib/parsers/logfmt_parser.hpp>

#include <test_common/log_format.hpp>
#include <test_common/log_generator.hpp>

#include <catch2/catch_all.hpp>

#include <cstddef>
#include <filesystem>

using namespace loglib;
using namespace bench;
using test_common::GenerateWideLogRecords;

namespace
{

// `LogfmtParser::ParseStreaming` adapter for `bench::RunStreamingBenchmark`.
// Free function (rather than a `bench::ParserStreamFn` global) to avoid
// `bugprone-throwing-static-initialization` from `std::function`'s ctor.
void LogfmtStream(
    FileLineSource &source, LogParseSink &sink, const ParserOptions &options, internal::AdvancedParserOptions advanced
)
{
    LogfmtParser::ParseStreaming(source, sink, options, advanced);
}

} // namespace

// Large-file streaming benchmark (1'000'000 lines). Mirrors the JSON
// `[large]` case. The streaming ctor avoids materializing the 1M records.
TEST_CASE("Stream logfmt log to LogTable (1'000'000 lines)", "[.][benchmark][logfmt_parser][large]")
{
    BENCHMARK_REQUIRES_RELEASE_BUILD();

    // Pinned seed + timestamps so the bytes match `[json_parser][large]`.
    const TestStructuredLogFile testFile(
        StreamedRecords{
            .count = 1'000'000, .seed = LARGE_FIXTURE_SEED, .timestamps = DeterministicBenchmarkTimestamps()
        },
        test_common::Logfmt()
    );
    const size_t bytes = std::filesystem::file_size(testFile.GetFilePath());

    InitializeTimezoneData();

    auto configuration = MakeTimestampConfiguration();
    const TestLogConfiguration configFile;
    configFile.Write(*configuration);

    RunStreamingBenchmark(
        "Stream 1'000'000 logfmt log entries to LogTable",
        configFile.GetFilePath(),
        testFile.GetFilePath(),
        configuration,
        LogfmtStream,
        testFile.RecordCount(),
        bytes,
        4
    );
}

// Wide-row streaming benchmark, mirror of `[json_parser][wide]`. Stresses
// the per-field hot loop in `LogfmtLineDecoder`. Nested array/object
// fields land as quoted JSON strings in logfmt (see `Logfmt()`), so the
// field count matches `[json_parser][wide]` but per-field work differs —
// treat cross-format lines/s as broadly comparable, not exactly.
TEST_CASE("Stream logfmt log to LogTable (wide, 200'000 lines)", "[.][benchmark][logfmt_parser][wide]")
{
    BENCHMARK_REQUIRES_RELEASE_BUILD();

    // Pinned seed + timestamps so the bytes match `[json_parser][wide]`.
    const TestStructuredLogFile testFile(
        GenerateWideLogRecords(200'000, /*columnCount=*/30, WIDE_FIXTURE_SEED, DeterministicBenchmarkTimestamps()),
        test_common::Logfmt()
    );
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    const size_t bytes = std::filesystem::file_size(testFile.GetFilePath());

    InitializeTimezoneData();

    auto configuration = MakeTimestampConfiguration();
    const TestLogConfiguration configFile;
    configFile.Write(*configuration);

    RunStreamingBenchmark(
        "Stream 200'000 wide logfmt log entries to LogTable",
        configFile.GetFilePath(),
        testFile.GetFilePath(),
        configuration,
        LogfmtStream,
        testFile.RecordCount(),
        bytes,
        4
    );
}
