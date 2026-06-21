// logfmt parser benchmarks for `loglib`. Mirrors the JSON `[large]` and
// `[wide]` streaming cases in `benchmark_json.cpp` through the shared
// `benchmark_common.hpp` harness so lines/s is directly comparable across
// formats (MB/s differs because logfmt bytes/line < JSON bytes/line). See
// CONTRIBUTING.md `## Benchmarking`. Debug builds skip these cases
// automatically — see `BENCHMARK_REQUIRES_RELEASE_BUILD`.

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

// logfmt parser plugged into the shared `RunStreamingBenchmark` harness.
const bench::ParserStreamFn kLogfmtStream =
    [](FileLineSource &source, LogParseSink &sink, const ParserOptions &options,
       internal::AdvancedParserOptions advanced) { LogfmtParser::ParseStreaming(source, sink, options, advanced); };

} // namespace

// Large-file streaming benchmark (1'000'000 lines). End-to-end GUI flow,
// same shape as the JSON `[large]` case: `LogTable::BeginStreaming` + a sink
// that calls `LogTable::AppendBatch` per `OnBatch`, with a `Type::Time`
// column so the streaming parser does real inline timestamp promotion. The
// fixture is generated through the streaming ctor so the 1M records are never
// materialized in RAM.
TEST_CASE("Stream logfmt log to LogTable (1'000'000 lines)", "[.][benchmark][logfmt_parser][large]")
{
    BENCHMARK_REQUIRES_RELEASE_BUILD();

    // Pinned seed (shared with `[json_parser][large]` via `LARGE_FIXTURE_SEED`)
    // so lines/s is directly comparable across formats and stable across runs.
    const TestStructuredLogFile testFile(
        StreamedRecords{.count = 1'000'000, .seed = LARGE_FIXTURE_SEED}, test_common::Logfmt()
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
        kLogfmtStream,
        testFile.RecordCount(),
        bytes,
        4
    );
}

// Wide-row streaming benchmark, mirror of `[json_parser][wide]`. Stresses
// the per-field hot loop in `LogfmtLineDecoder` (key scan, value-shape
// classification, optional quote/unescape). The same `GenerateWideLogRecords`
// fixture feeds both formats; the wide-row generator emits a handful of
// nested array / object fields that `test_common::Logfmt()` flattens into
// quoted JSON strings (see `test/common/include/test_common/log_format.hpp`).
// Per-line field count therefore matches `[json_parser][wide]` (~30 fields),
// though the cost the parser pays per nested field differs — JSON sees
// structured values, logfmt sees an opaque quoted string. Treat the
// cross-format lines/s as broadly comparable rather than directly so.
TEST_CASE("Stream logfmt log to LogTable (wide, 200'000 lines)", "[.][benchmark][logfmt_parser][wide]")
{
    BENCHMARK_REQUIRES_RELEASE_BUILD();

    // Pinned seed (shared with `[json_parser][wide]` via `WIDE_FIXTURE_SEED`)
    // so the two formats consume byte-identical record sequences.
    const TestStructuredLogFile testFile(
        GenerateWideLogRecords(200'000, /*columnCount=*/30, WIDE_FIXTURE_SEED), test_common::Logfmt()
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
        kLogfmtStream,
        testFile.RecordCount(),
        bytes,
        4
    );
}
