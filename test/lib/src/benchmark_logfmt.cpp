// logfmt parser benchmarks. Mirror of the JSON large and wide streaming benchmarks through the shared
// `benchmark_common.hpp` harness; lines/s is directly comparable across formats (MB/s differs because logfmt has
// fewer bytes/line than JSON). See CONTRIBUTING.md `## Benchmarking`.

#include "benchmark_common.hpp"
#include "benchmark_registry.hpp"
#include "common.hpp"

#include <loglib/core/file_line_source.hpp>
#include <loglib/parse/log_parse_sink.hpp>
#include <loglib/parse/parser_options.hpp>
#include <loglib/parse/parsers/logfmt_parser.hpp>

#include <test_common/log_format.hpp>
#include <test_common/log_generator.hpp>

#include <benchmark/benchmark.h>

#include <memory>

using namespace loglib;
using test_common::GenerateWideLogRecords;

namespace bench
{

namespace
{

// `LogfmtParser::ParseStreaming` adapter for `RegisterStreaming`. Free function (rather than a `ParserStreamFn`
// global) to avoid `bugprone-throwing-static-initialization` from `std::function`'s ctor.
void LogfmtStream(FileLineSource &source, LogParseSink &sink, const ParserOptions &options)
{
    LogfmtParser{}.ParseStreaming(source, sink, options);
}

} // namespace

void RegisterLogfmtBenchmarks()
{
    // Large-file streaming benchmark (1'000'000 lines). Mirrors the JSON one. The streaming constructor avoids
    // materializing the 1M records, and the pinned seed and timestamps make the bytes match the JSON fixture.
    RegisterStreaming("Stream 1'000'000 logfmt log entries to LogTable", []() {
        return StreamingFixture{
            .logFile = std::make_unique<TestStructuredLogFile>(
                StreamedRecords{
                    .count = 1'000'000, .seed = LARGE_FIXTURE_SEED, .timestamps = DeterministicBenchmarkTimestamps()
                },
                test_common::Logfmt()
            ),
            .parser = LogfmtStream,
        };
    });

    // Wide-row streaming benchmark, mirror of the JSON one. Stresses the per-field hot loop in `LogfmtLineDecoder`.
    // Nested array/object fields land as quoted JSON strings in logfmt (see `Logfmt()`), so the field count matches
    // the JSON benchmark but per-field work differs -- treat cross-format lines/s as broadly comparable, not exactly.
    RegisterStreaming("Stream 200'000 wide logfmt log entries to LogTable", []() {
        return StreamingFixture{
            .logFile = std::make_unique<TestStructuredLogFile>(
                GenerateWideLogRecords(
                    200'000, /*columnCount=*/30, WIDE_FIXTURE_SEED, DeterministicBenchmarkTimestamps()
                ),
                test_common::Logfmt()
            ),
            .parser = LogfmtStream,
        };
    });
}

} // namespace bench
