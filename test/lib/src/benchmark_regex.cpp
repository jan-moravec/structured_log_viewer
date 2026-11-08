// RegexParser benchmarks; mirror of the JSON, logfmt and CSV large streaming benchmarks via the shared
// benchmark_common.hpp. One large benchmark per shipped test_common::LogFormat synthesizer, so each streams lines
// that the corresponding real RegexTemplate pattern parses.
//
// Lines/s is the primary regression-gate metric and is directly comparable across templates. MB/s isn't -- each
// format's per-record punctuation lands files at different byte sizes. See CONTRIBUTING.md ## Benchmarking.

#include "benchmark_common.hpp"
#include "benchmark_registry.hpp"
#include "common.hpp"

#include <loglib/core/file_line_source.hpp>
#include <loglib/parse/log_parse_sink.hpp>
#include <loglib/parse/parser_options.hpp>
#include <loglib/parse/parsers/regex_parser.hpp>
#include <loglib/parse/regex_templates.hpp>

#include <test_common/log_format.hpp>
#include <test_common/log_generator.hpp>

#include <benchmark/benchmark.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace loglib;

namespace bench
{

namespace
{

constexpr std::size_t REGEX_BENCH_LINES = 1'000'000;

// Streams a fixture produced by @p factory through the shipped template called @p templateName. The pattern is copied
// into a local string so the parser closure outlives every run.
StreamingFixture MakeTemplateFixture(
    std::string_view templateName, test_common::LogFormat (*factory)(), const std::string &fileName
)
{
    const RegexTemplate *tmpl = FindTemplateByName(templateName);
    if (tmpl == nullptr)
    {
        throw std::runtime_error("regex template not found: " + std::string(templateName));
    }
    const std::string pattern{tmpl->pattern};
    const ContinuationMode continuation = tmpl->continuationMode;
    const std::string headerAnchor{tmpl->headerAnchor};
    return StreamingFixture{
        .logFile = std::make_unique<TestStructuredLogFile>(
            StreamedRecords{
                .count = REGEX_BENCH_LINES,
                .seed = LARGE_FIXTURE_SEED,
                .timestamps = DeterministicBenchmarkTimestamps(),
            },
            factory(),
            test_common::RecordSchema{},
            fileName
        ),
        .parser = [pattern, continuation, headerAnchor](
                      FileLineSource &source, LogParseSink &sink, const ParserOptions &options
                  ) { RegexParser{pattern, continuation, headerAnchor}.ParseStreaming(source, sink, options); },
    };
}

void RegisterTemplateBenchmark(
    const std::string &name, std::string_view templateName, test_common::LogFormat (*factory)(), std::string fileName
)
{
    RegisterStreaming(name, [templateName, factory, fileName = std::move(fileName)] {
        return MakeTemplateFixture(templateName, factory, fileName);
    });
}

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
constexpr const char PYTHON_TRACEBACK_ANCHOR[] = R"(^(?:TRACE|DEBUG|INFO|WARN|WARNING|ERROR|CRITICAL|FATAL): )";

// Registers one extra Python-traceback template for as long as it lives, and removes it afterwards.
class ExtraTemplateGuard
{
public:
    ExtraTemplateGuard(std::string name, std::string headerAnchor)
        : mTemplate{
              .name = std::move(name),
              .pattern = PYTHON_TRACEBACK_PATTERN,
              .sampleLines = {"INFO: hello"},
              .autoDetect = false,
              .priority = loglib::USER_TEMPLATE_DEFAULT_PRIORITY,
              .description = "",
              .continuationMode = ContinuationMode::UntilNextHeader,
              .headerAnchor = std::move(headerAnchor),
          }
    {
        mExtras.push_back(mTemplate);
        loglib::SetExtraRegexTemplates(std::span<const RegexTemplate>(mExtras));
    }
    ExtraTemplateGuard(const ExtraTemplateGuard &) = delete;
    ExtraTemplateGuard(ExtraTemplateGuard &&) = delete;
    ExtraTemplateGuard &operator=(const ExtraTemplateGuard &) = delete;
    ExtraTemplateGuard &operator=(ExtraTemplateGuard &&) = delete;
    ~ExtraTemplateGuard()
    {
        loglib::SetExtraRegexTemplates({});
    }

    const RegexTemplate &Template() const noexcept
    {
        return mTemplate;
    }

private:
    RegexTemplate mTemplate;
    std::vector<RegexTemplate> mExtras;
};

StreamingFixture MakePythonTracebackFixture(
    const std::string &name, const std::string &headerAnchor, const std::string &fileName
)
{
    const auto guard = std::make_shared<ExtraTemplateGuard>(name, headerAnchor);
    const RegexTemplate &extra = guard->Template();
    return StreamingFixture{
        .logFile = std::make_unique<TestStructuredLogFile>(
            StreamedRecords{
                .count = REGEX_BENCH_LINES,
                .seed = LARGE_FIXTURE_SEED,
                .timestamps = DeterministicBenchmarkTimestamps(),
            },
            PythonTracebackFormat(/*everyN=*/10),
            test_common::RecordSchema{},
            fileName
        ),
        .parser =
            [guard, &extra](FileLineSource &source, LogParseSink &sink, const ParserOptions &options) {
                RegexParser{extra.pattern, extra.continuationMode, extra.headerAnchor}.ParseStreaming(
                    source, sink, options
                );
            },
        .keepAlive = guard,
    };
}

} // namespace

void RegisterRegexBenchmarks()
{
    RegisterTemplateBenchmark(
        "Stream 1'000'000 Syslog (RFC3164) entries to LogTable",
        "Syslog (RFC3164)",
        &test_common::SyslogRfc3164Format,
        "bench_regex_syslog.log"
    );
    RegisterTemplateBenchmark(
        "Stream 1'000'000 Apache/nginx Combined entries to LogTable",
        "Apache/nginx Combined Log Format",
        &test_common::ApacheCombinedFormat,
        "bench_regex_apache_combined.log"
    );
    RegisterTemplateBenchmark(
        "Stream 1'000'000 Apache/nginx Common entries to LogTable",
        "Apache/nginx Common Log Format",
        &test_common::ApacheCommonFormat,
        "bench_regex_apache_common.log"
    );
    RegisterTemplateBenchmark(
        "Stream 1'000'000 Apache error entries to LogTable",
        "Apache error log",
        &test_common::ApacheErrorFormat,
        "bench_regex_apache_error.log"
    );
    RegisterTemplateBenchmark(
        "Stream 1'000'000 Java / log4j / SLF4J entries to LogTable",
        "Java / log4j / SLF4J Logback",
        &test_common::JavaLogFormat,
        "bench_regex_java_log.log"
    );

    RegisterStreaming("Stream 1'000'000 Java multi-line (indented) records to LogTable", [] {
        const RegexTemplate *tmpl = FindTemplateByName("Java / log4j / SLF4J Logback");
        if (tmpl == nullptr || tmpl->continuationMode != ContinuationMode::Indented)
        {
            throw std::runtime_error("the Java template is missing or not indented");
        }
        const std::string pattern{tmpl->pattern};
        const std::string headerAnchor{tmpl->headerAnchor};
        return StreamingFixture{
            .logFile = std::make_unique<TestStructuredLogFile>(
                StreamedRecords{
                    .count = REGEX_BENCH_LINES,
                    .seed = LARGE_FIXTURE_SEED,
                    .timestamps = DeterministicBenchmarkTimestamps(),
                },
                MultilineIndentedFormat(test_common::JavaLogFormat(), /*everyN=*/10),
                test_common::RecordSchema{},
                "bench_regex_java_multiline.log"
            ),
            .parser = [pattern,
                       headerAnchor](FileLineSource &source, LogParseSink &sink, const ParserOptions &options) {
                RegexParser{pattern, ContinuationMode::Indented, headerAnchor}.ParseStreaming(source, sink, options);
            },
        };
    });

    RegisterStreaming("Stream 1'000'000 Python traceback (UntilNextHeader) records to LogTable", [] {
        return MakePythonTracebackFixture("test-python-traceback", "", "bench_regex_python_traceback.log");
    });

    // The header anchor lets the parser recognise a header line without running the full pattern. The two
    // benchmarks parse the same file with and without the anchor and time only the parse loop (LogTable::AppendBatch
    // excluded), so the gap between them is the speedup of the anchor.
    RegisterStreaming(
        "Python-traceback header anchor (parse-loop only): baseline (main-pattern probe)",
        [] {
            return MakePythonTracebackFixture("test-python-traceback-anchor", "", "bench_regex_header_baseline.log");
        },
        /*parseLoopOnly=*/true,
        FAST_LARGE_ITERATIONS
    );
    RegisterStreaming(
        "Python-traceback header anchor (parse-loop only): anchored",
        [] {
            return MakePythonTracebackFixture(
                "test-python-traceback-anchor", PYTHON_TRACEBACK_ANCHOR, "bench_regex_header_anchored.log"
            );
        },
        /*parseLoopOnly=*/true,
        FAST_LARGE_ITERATIONS
    );
}

} // namespace bench
