// RegexParser benchmarks; mirror of `[json_parser]` /
// `[logfmt_parser]` / `[csv_parser]` `[large]` via the shared
// `benchmark_common.hpp`. One `[large]` case per shipped
// `test_common::LogFormat` synthesizer, so each streams lines that
// the corresponding real `RegexTemplate` pattern parses (rather
// than the retired bracketed-regex placeholder).
//
// Lines/s is the primary regression-gate metric and is directly
// comparable across templates. MB/s isn't — each format's
// per-record punctuation lands files at different byte sizes.
// See CONTRIBUTING.md `## Benchmarking`.

#include "benchmark_common.hpp"
#include "common.hpp"

#include <loglib/file_line_source.hpp>
#include <loglib/internal/advanced_parser_options.hpp>
#include <loglib/log_parse_sink.hpp>
#include <loglib/parser_options.hpp>
#include <loglib/parsers/regex_parser.hpp>
#include <loglib/regex_templates.hpp>

#include <test_common/log_format.hpp>
#include <test_common/log_generator.hpp>

#include <catch2/catch_all.hpp>

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

using namespace loglib;
using namespace bench;

namespace
{

/// Shared per-template streaming benchmark. Materialises a 1M-line
/// fixture through @p factory, then drives @p templateName's
/// pattern via `RegexParser::ParseStreaming` through the shared
/// harness.
///
/// The pattern is copied into a local `std::string` so the closure
/// captures a reference that outlives every `RunStreamingFlow`
/// sample. Registry storage is process-lifetime, but a defensive
/// owned copy keeps the closure state-free.
void RunRegexTemplateBenchmark(
    std::string_view templateName,
    test_common::LogFormat (*factory)(),
    const char *label,
    const std::filesystem::path &logPath,
    std::size_t lines,
    std::size_t samples
)
{
    BENCHMARK_REQUIRES_RELEASE_BUILD();

    const RegexTemplate *tmpl = FindTemplateByName(templateName);
    REQUIRE(tmpl != nullptr);
    // Owned copy: the closure captures by reference; this string
    // outlives every `RegexParser::ParseStreaming` invocation.
    const std::string pattern{tmpl->pattern};

    const test_common::TimestampPolicy timestamps = DeterministicBenchmarkTimestamps();

    const TestStructuredLogFile testFile(
        StreamedRecords{.count = lines, .seed = LARGE_FIXTURE_SEED, .timestamps = timestamps},
        factory(),
        test_common::RecordSchema{},
        logPath.string()
    );
    const std::size_t bytes = std::filesystem::file_size(testFile.GetFilePath());

    InitializeTimezoneData();

    auto configuration = MakeTimestampConfiguration();
    const TestLogConfiguration configFile;
    configFile.Write(*configuration);

    const ParserStreamFn parserStream = [&pattern](
                                            FileLineSource &source,
                                            LogParseSink &sink,
                                            const ParserOptions &options,
                                            internal::AdvancedParserOptions advanced
                                        ) { RegexParser::ParseStreaming(source, sink, options, advanced, pattern); };

    RunStreamingBenchmark(
        label,
        configFile.GetFilePath(),
        testFile.GetFilePath(),
        configuration,
        parserStream,
        testFile.RecordCount(),
        bytes,
        samples
    );
}

constexpr std::size_t REGEX_BENCH_LINES = 1'000'000;
constexpr std::size_t REGEX_BENCH_SAMPLES = 4;

} // namespace

TEST_CASE("Stream Syslog (RFC3164) log to LogTable (1'000'000 lines)", "[.][benchmark][regex_parser][large]")
{
    RunRegexTemplateBenchmark(
        "Syslog (RFC3164)",
        &test_common::SyslogRfc3164Format,
        "Stream 1'000'000 Syslog (RFC3164) entries to LogTable",
        "bench_regex_syslog.log",
        REGEX_BENCH_LINES,
        REGEX_BENCH_SAMPLES
    );
}

TEST_CASE(
    "Stream Apache/nginx Combined Log Format log to LogTable (1'000'000 lines)", "[.][benchmark][regex_parser][large]"
)
{
    RunRegexTemplateBenchmark(
        "Apache/nginx Combined Log Format",
        &test_common::ApacheCombinedFormat,
        "Stream 1'000'000 Apache/nginx Combined entries to LogTable",
        "bench_regex_apache_combined.log",
        REGEX_BENCH_LINES,
        REGEX_BENCH_SAMPLES
    );
}

TEST_CASE(
    "Stream Apache/nginx Common Log Format log to LogTable (1'000'000 lines)", "[.][benchmark][regex_parser][large]"
)
{
    RunRegexTemplateBenchmark(
        "Apache/nginx Common Log Format",
        &test_common::ApacheCommonFormat,
        "Stream 1'000'000 Apache/nginx Common entries to LogTable",
        "bench_regex_apache_common.log",
        REGEX_BENCH_LINES,
        REGEX_BENCH_SAMPLES
    );
}

TEST_CASE("Stream Apache error log to LogTable (1'000'000 lines)", "[.][benchmark][regex_parser][large]")
{
    RunRegexTemplateBenchmark(
        "Apache error log",
        &test_common::ApacheErrorFormat,
        "Stream 1'000'000 Apache error entries to LogTable",
        "bench_regex_apache_error.log",
        REGEX_BENCH_LINES,
        REGEX_BENCH_SAMPLES
    );
}

TEST_CASE(
    "Stream Java / log4j / SLF4J Logback log to LogTable (1'000'000 lines)", "[.][benchmark][regex_parser][large]"
)
{
    RunRegexTemplateBenchmark(
        "Java / log4j / SLF4J Logback",
        &test_common::JavaLogFormat,
        "Stream 1'000'000 Java / log4j / SLF4J entries to LogTable",
        "bench_regex_java_log.log",
        REGEX_BENCH_LINES,
        REGEX_BENCH_SAMPLES
    );
}
