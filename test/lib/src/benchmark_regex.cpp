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

// ---------------------------------------------------------------------------
// Multi-line benchmarks
// ---------------------------------------------------------------------------
//
// The two cases below exercise `RegexLineDecoder`'s multi-line paths on
// 1 M-record fixtures. Every 10th record carries a synthetic stack trace
// (three indented frames) so ~30 % of physical lines are continuations.
// Numbers are directly comparable to the single-line Java case above:
//   * `[java_multiline]` measures the `Indented` mode cost (one-byte
//     header check per line).
//   * `[untilNextHeader]` measures R5 (`PCRE2_ANCHORED | PCRE2_PARTIAL_HARD`
//     probe per line) using a hand-authored Python-traceback-shaped
//     template registered via `SetExtraRegexTemplates`.
//
// Acceptance bar (see CONTRIBUTING.md `## Benchmarking`): both cases
// stay within ±3 % of the single-line Java case's lines/s on the same
// hardware, i.e. the multi-line machinery must not regress the hot path
// for records that never continue.

namespace
{

/// Local counter-based "every N" gate. `writeLine` is called
/// sequentially from `TestStructuredLogFile`'s fixture-generation loop
/// (single-threaded) so a mutable captured counter is race-free.
struct EveryN
{
    std::size_t stride = 10;
    std::size_t counter = 0;
    bool tick() noexcept
    {
        const bool fire = stride != 0 && ((counter % stride) == (stride - 1));
        ++counter;
        return fire;
    }
};

/// Look up @p key inside @p record's object. Returns "" if the record
/// is not an object or the key is missing / non-string. Duplicated
/// from `log_format.cpp`'s private `FieldOr` so the benchmark stays
/// self-contained without widening `test_common`'s public surface.
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

/// Wrap @p base so every @p everyN-th record's line is followed by a
/// synthetic multi-line stack trace (3 indented frames). Continuation
/// lines are legal inside `writeLine` output — `TestStructuredLogFile`
/// only asserts the string does not END on '\n'.
test_common::LogFormat MultilineIndentedFormat(test_common::LogFormat base, std::size_t everyN)
{
    // Shared mutable state; the closure owns it and the writer loop is
    // single-threaded so no atomic is required.
    auto gate = std::make_shared<EveryN>(EveryN{.stride = everyN});
    return test_common::LogFormat{
        .suggestedExtension = base.suggestedExtension,
        .writeHeader = base.writeHeader,
        .writeLine =
            [base = std::move(base), gate](const test_common::LogRecord &record) {
                std::string out = base.writeLine(record);
                if (gate->tick())
                {
                    out += "\n\tat com.example.Foo.bar(Foo.java:42)"
                           "\n\tat com.example.Baz.qux(Baz.java:73)"
                           "\n\tat com.example.Quux.run(Quux.java:15)";
                }
                return out;
            },
    };
}

/// Python-traceback-shaped format. Every 10th record spawns a synthetic
/// traceback:
///
///     Traceback (most recent call last):
///       File "foo.py", line 12, in <module>
///         raise ValueError('x')
///     During handling of the above exception, another exception occurred:
///       File "foo.py", line 20, in <module>
///         wrap()
///
/// The mid-block "During handling..." line is un-indented, which is
/// exactly what forces `UntilNextHeader` mode (an `Indented`-only rule
/// would stop folding at that line).
test_common::LogFormat PythonTracebackFormat(std::size_t everyN)
{
    auto gate = std::make_shared<EveryN>(EveryN{.stride = everyN});
    return test_common::LogFormat{
        .suggestedExtension = ".log",
        .writeHeader = [](const test_common::RecordSchema &) { return std::string{}; },
        .writeLine =
            [gate](const test_common::LogRecord &record) {
                // Single-line header shape parsed by the template
                // registered below: `LEVEL: message`.
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
                if (gate->tick())
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

/// Python-traceback header pattern. Two named groups (level + message)
/// so `LastContinuationTarget` folds continuation bytes into `message`
/// -- the last named group in source order.
constexpr const char PYTHON_TRACEBACK_PATTERN[] = R"(^(?<level>DEBUG|INFO|WARN|WARNING|ERROR|CRITICAL): (?<message>.*)$)";

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

    // Register a Python-traceback template with `UntilNextHeader`
    // mode so `FindTemplateByPattern` in `ParseStreaming` picks it up
    // and drives R5's `PCRE2_ANCHORED | PCRE2_PARTIAL_HARD` probe on
    // every non-header line.
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
        ~ExtrasGuard()
        {
            loglib::SetExtraRegexTemplates({});
        }
    } guard;
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
