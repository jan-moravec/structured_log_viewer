// RegexParser benchmark; mirror of `[json_parser]` / `[logfmt_parser]` /
// `[csv_parser]` `[large]` via the shared `benchmark_common.hpp`. The
// fixture is the same `GenerateRandomLogRecord` sequence used by the
// other formats, serialised through `BracketedRegex()` so lines/s is
// directly comparable; MB/s is not (the bracketed shape has more
// punctuation than logfmt/csv per record). See CONTRIBUTING.md
// `## Benchmarking`.

#include "benchmark_common.hpp"
#include "common.hpp"

#include <loglib/file_line_source.hpp>
#include <loglib/internal/advanced_parser_options.hpp>
#include <loglib/log_parse_sink.hpp>
#include <loglib/parser_options.hpp>
#include <loglib/parsers/regex_parser.hpp>

#include <test_common/log_format.hpp>
#include <test_common/log_generator.hpp>

#include <catch2/catch_all.hpp>

#include <cstddef>
#include <filesystem>
#include <string>

using namespace loglib;
using namespace bench;

namespace
{

// Free-function adapter (a `bench::ParserStreamFn` global would trip
// `bugprone-throwing-static-initialization` from `std::function`'s ctor).
// Pinned to the bracketed-regex pattern paired with the wire format.
void RegexStream(
    FileLineSource &source,
    LogParseSink &sink,
    const ParserOptions &options,
    internal::AdvancedParserOptions advanced
)
{
    RegexParser::ParseStreaming(source, sink, options, advanced, test_common::BracketedRegexPattern());
}

} // namespace

// Large-file streaming benchmark, mirror of `[json_parser]` /
// `[logfmt_parser]` / `[csv_parser]` `[large]`. Records stream
// straight to disk -- no 1M-record vector ever materialises.
TEST_CASE("Stream regex log to LogTable (1'000'000 lines)", "[.][benchmark][regex_parser][large]")
{
    BENCHMARK_REQUIRES_RELEASE_BUILD();

    const test_common::TimestampPolicy timestamps = DeterministicBenchmarkTimestamps();

    const TestStructuredLogFile testFile(
        StreamedRecords{.count = 1'000'000, .seed = LARGE_FIXTURE_SEED, .timestamps = timestamps},
        test_common::BracketedRegex(),
        test_common::RecordSchema{}
    );
    const size_t bytes = std::filesystem::file_size(testFile.GetFilePath());

    InitializeTimezoneData();

    auto configuration = MakeTimestampConfiguration();
    const TestLogConfiguration configFile;
    configFile.Write(*configuration);

    RunStreamingBenchmark(
        "Stream 1'000'000 regex log entries to LogTable",
        configFile.GetFilePath(),
        testFile.GetFilePath(),
        configuration,
        RegexStream,
        testFile.RecordCount(),
        bytes,
        4
    );
}
