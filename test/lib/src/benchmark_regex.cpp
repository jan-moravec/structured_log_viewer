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

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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

namespace
{

struct EveryN
{
    std::size_t stride = 10;
    std::size_t counter = 0;
    bool Tick() noexcept
    {
        const bool fire = stride != 0 && ((counter % stride) == (stride - 1));
        ++counter;
        return fire;
    }
};

std::string FieldOrEmpty(const test_common::LogRecord &record, std::string_view key)
{
    if (!record.is_object())
    {
        return {};
    }
    for (const auto &[k, v] : record.get_object())
    {
        if (k == key && v.is_string())
        {
            return v.get_string();
        }
    }
    return {};
}

test_common::LogFormat MultilineIndentedFormat(test_common::LogFormat base, std::size_t everyN)
{
    auto gate = std::make_shared<EveryN>(EveryN{.stride = everyN});
    return test_common::LogFormat{
        .suggestedExtension = base.suggestedExtension,
        .writeHeader = base.writeHeader,
        .writeLine = [base = std::move(base), gate](const test_common::LogRecord &record) {
            std::string out = base.writeLine(record);
            if (gate->Tick())
            {
                out += "\n\tat com.example.Foo.bar(Foo.java:42)"
                       "\n\tat com.example.Baz.qux(Baz.java:73)"
                       "\n\tat com.example.Quux.run(Quux.java:15)";
            }
            return out;
        },
    };
}

test_common::LogFormat PythonTracebackFormat(std::size_t everyN)
{
    auto gate = std::make_shared<EveryN>(EveryN{.stride = everyN});
    return test_common::LogFormat{
        .suggestedExtension = ".log",
        .writeHeader = [](const test_common::RecordSchema &) { return std::string{}; },
        .writeLine =
            [gate](const test_common::LogRecord &record) {
                std::string level = FieldOrEmpty(record, "level");
                if (level.empty())
                {
                    level = "info";
                }
                std::for_each(level.begin(), level.end(), [](char &c) {
                    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                });
                std::string message = FieldOrEmpty(record, "message");
                if (message.empty())
                {
                    message = "message";
                }
                std::string out = level + ": " + message;
                if (gate->Tick())
                {
                    out += "\nTraceback (most recent call last):"
                           "\n  File \"foo.py\", line 12, in <module>"
                           "\n    raise ValueError('x')"
                           "\nValueError: x"
                           "\nDuring handling of the above exception, another exception occurred:"
                           "\n  File \"foo.py\", line 20, in <module>"
                           "\n    wrap()"
                           "\nRuntimeError: nested";
                }
                return out;
            },
    };
}

constexpr const char PYTHON_TRACEBACK_PATTERN[] =
    R"(^(?<level>TRACE|DEBUG|INFO|WARN|WARNING|ERROR|CRITICAL|FATAL): (?<message>.*)$)";

} // namespace

TEST_CASE(
    "Stream Java multi-line (indented) log to LogTable (1'000'000 records)",
    "[.][benchmark][regex_parser][large][java_multiline]"
)
{
    BENCHMARK_REQUIRES_RELEASE_BUILD();

    const RegexTemplate *tmpl = FindTemplateByName("Java / log4j / SLF4J Logback");
    REQUIRE(tmpl != nullptr);
    REQUIRE(tmpl->continuationMode == ContinuationMode::Indented);
    const std::string pattern{tmpl->pattern};

    const test_common::TimestampPolicy timestamps = DeterministicBenchmarkTimestamps();

    const TestStructuredLogFile testFile(
        StreamedRecords{.count = REGEX_BENCH_LINES, .seed = LARGE_FIXTURE_SEED, .timestamps = timestamps},
        MultilineIndentedFormat(test_common::JavaLogFormat(), /*everyN=*/10),
        test_common::RecordSchema{},
        "bench_regex_java_multiline.log"
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
        "Stream 1'000'000 Java multi-line (indented) records to LogTable",
        configFile.GetFilePath(),
        testFile.GetFilePath(),
        configuration,
        parserStream,
        testFile.RecordCount(),
        bytes,
        REGEX_BENCH_SAMPLES
    );
}

TEST_CASE(
    "Stream Python-traceback (UntilNextHeader) log to LogTable (1'000'000 records)",
    "[.][benchmark][regex_parser][large][untilNextHeader]"
)
{
    BENCHMARK_REQUIRES_RELEASE_BUILD();

    const std::string pattern{PYTHON_TRACEBACK_PATTERN};
    const RegexTemplate extra{
        .name = "test-python-traceback",
        .pattern = pattern,
        .sampleLines = {"INFO: hello"},
        .autoDetect = false,
        .priority = loglib::USER_TEMPLATE_DEFAULT_PRIORITY,
        .description = "",
        .continuationMode = ContinuationMode::UntilNextHeader,
        .headerAnchor = "",
    };
    const std::vector<RegexTemplate> extras{extra};
    loglib::SetExtraRegexTemplates(std::span<const RegexTemplate>(extras));
    struct ExtrasGuard
    {
        ExtrasGuard() = default;
        ExtrasGuard(const ExtrasGuard &) = delete;
        ExtrasGuard(ExtrasGuard &&) = delete;
        ExtrasGuard &operator=(const ExtrasGuard &) = delete;
        ExtrasGuard &operator=(ExtrasGuard &&) = delete;
        ~ExtrasGuard()
        {
            loglib::SetExtraRegexTemplates({});
        }
    };
    const ExtrasGuard guard;
    (void)guard;

    const test_common::TimestampPolicy timestamps = DeterministicBenchmarkTimestamps();

    const TestStructuredLogFile testFile(
        StreamedRecords{.count = REGEX_BENCH_LINES, .seed = LARGE_FIXTURE_SEED, .timestamps = timestamps},
        PythonTracebackFormat(/*everyN=*/10),
        test_common::RecordSchema{},
        "bench_regex_python_traceback.log"
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
        "Stream 1'000'000 Python traceback (UntilNextHeader) records to LogTable",
        configFile.GetFilePath(),
        testFile.GetFilePath(),
        configuration,
        parserStream,
        testFile.RecordCount(),
        bytes,
        REGEX_BENCH_SAMPLES
    );
}

TEST_CASE(
    "Python-traceback (UntilNextHeader) with headerAnchor beats main-pattern probe (1'000'000 records)",
    "[.][benchmark][regex_parser][large][header_anchor]"
)
{
    BENCHMARK_REQUIRES_RELEASE_BUILD();

    const std::string pattern{PYTHON_TRACEBACK_PATTERN};
    const std::string anchor = R"(^(?:TRACE|DEBUG|INFO|WARN|WARNING|ERROR|CRITICAL|FATAL): )";

    const test_common::TimestampPolicy timestamps = DeterministicBenchmarkTimestamps();

    const TestStructuredLogFile testFile(
        StreamedRecords{.count = REGEX_BENCH_LINES, .seed = LARGE_FIXTURE_SEED, .timestamps = timestamps},
        PythonTracebackFormat(/*everyN=*/10),
        test_common::RecordSchema{},
        "bench_regex_header_anchor.log"
    );
    const std::size_t bytes = std::filesystem::file_size(testFile.GetFilePath());

    InitializeTimezoneData();

    auto configuration = MakeTimestampConfiguration();
    const TestLogConfiguration configFile;
    configFile.Write(*configuration);

    {
        const RegexTemplate extraNoAnchor{
            .name = "test-python-traceback-anchor",
            .pattern = pattern,
            .sampleLines = {"INFO: hello"},
            .autoDetect = false,
            .priority = loglib::USER_TEMPLATE_DEFAULT_PRIORITY,
            .description = "",
            .continuationMode = ContinuationMode::UntilNextHeader,
            .headerAnchor = "",
        };
        const std::vector<RegexTemplate> extras{extraNoAnchor};
        loglib::SetExtraRegexTemplates(std::span<const RegexTemplate>(extras));
        const ParserStreamFn parserStream = [&pattern](
                                                FileLineSource &source,
                                                LogParseSink &sink,
                                                const ParserOptions &options,
                                                internal::AdvancedParserOptions advanced
                                            ) {
            RegexParser::ParseStreaming(source, sink, options, advanced, pattern);
        };
        const StreamingRunResult warmup =
            RunStreamingFlow(configFile.GetFilePath(), testFile.GetFilePath(), configuration, parserStream);
        REQUIRE(warmup.rowCount == testFile.RecordCount());
        loglib::SetExtraRegexTemplates({});
    }

    auto runOneConfig = [&](const std::string &headerAnchor) -> std::chrono::nanoseconds {
        const RegexTemplate extra{
            .name = "test-python-traceback-anchor",
            .pattern = pattern,
            .sampleLines = {"INFO: hello"},
            .autoDetect = false,
            .priority = loglib::USER_TEMPLATE_DEFAULT_PRIORITY,
            .description = "",
            .continuationMode = ContinuationMode::UntilNextHeader,
            .headerAnchor = headerAnchor,
        };
        const std::vector<RegexTemplate> extras{extra};
        loglib::SetExtraRegexTemplates(std::span<const RegexTemplate>(extras));

        const ParserStreamFn parserStream = [&pattern](
                                                FileLineSource &source,
                                                LogParseSink &sink,
                                                const ParserOptions &options,
                                                internal::AdvancedParserOptions advanced
                                            ) {
            RegexParser::ParseStreaming(source, sink, options, advanced, pattern);
        };

        const StreamingRunResult run =
            RunStreamingFlow(configFile.GetFilePath(), testFile.GetFilePath(), configuration, parserStream);
        REQUIRE(run.rowCount == testFile.RecordCount());
        loglib::SetExtraRegexTemplates({});
        return run.elapsed - run.appendTotal;
    };

    constexpr std::size_t HEADER_ANCHOR_SAMPLES = REGEX_BENCH_SAMPLES * 2;
    std::vector<std::chrono::nanoseconds> baselineSamples;
    std::vector<std::chrono::nanoseconds> anchoredSamples;
    baselineSamples.reserve(HEADER_ANCHOR_SAMPLES);
    anchoredSamples.reserve(HEADER_ANCHOR_SAMPLES);
    for (std::size_t i = 0; i < HEADER_ANCHOR_SAMPLES; ++i)
    {
        baselineSamples.push_back(runOneConfig(""));
        anchoredSamples.push_back(runOneConfig(anchor));
    }

    const auto meanOf = [](const std::vector<std::chrono::nanoseconds> &s) {
        const auto sum = std::accumulate(s.begin(), s.end(), std::chrono::nanoseconds::zero());
        return sum / static_cast<long long>(s.size());
    };
    const auto baselineMean = meanOf(baselineSamples);
    const auto anchoredMean = meanOf(anchoredSamples);

    const double bytesMB = static_cast<double>(bytes) / (1024.0 * 1024.0);
    const double baselineSec = std::chrono::duration<double>(baselineMean).count();
    const double anchoredSec = std::chrono::duration<double>(anchoredMean).count();
    const double baselineMBps = baselineSec == 0.0 ? 0.0 : bytesMB / baselineSec;
    const double anchoredMBps = anchoredSec == 0.0 ? 0.0 : bytesMB / anchoredSec;
    const double ratio = baselineMBps == 0.0 ? 0.0 : anchoredMBps / baselineMBps;
    WARN(
        "Python-traceback header anchor (parse-loop only): baseline (main-pattern probe) "
        << baselineMBps << " MB/s vs anchored " << anchoredMBps << " MB/s => " << ratio << "x speedup"
    );

    CHECK(ratio >= 1.03);
}
