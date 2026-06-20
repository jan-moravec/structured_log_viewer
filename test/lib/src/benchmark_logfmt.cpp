// logfmt parser benchmarks for `loglib`. Mirrors the JSON `[large]`
// streaming case in `benchmark_json.cpp` through the shared
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

#include <catch2/catch_all.hpp>

#include <cstddef>
#include <filesystem>

using namespace loglib;
using namespace bench;

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

    const TestStructuredLogFile testFile(StreamedRecords{.count = 1'000'000}, test_common::Logfmt());
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
