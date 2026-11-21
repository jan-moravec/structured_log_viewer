#include <loglib/config/log_configuration.hpp>
#include <loglib/parse/format_detection.hpp>
#include <loglib/parse/log_parser.hpp>
#include <loglib/parse/regex_templates.hpp>

#include <test_common/temp_dir.hpp>

#include <catch2/catch_all.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

using loglib::DetectFormat;
using loglib::FormatSpec;
using loglib::MakeParser;
using loglib::ReadProbeHead;
using loglib::RegexTemplate;
using loglib::RegexTemplateCatalog;
using loglib::SampleFitsFormat;
using test_common::TempDir;

namespace
{

FormatSpec DetectFile(const std::filesystem::path &file)
{
    return DetectFormat(ReadProbeHead(file)).value_or(FormatSpec{});
}

} // namespace

TEST_CASE("DetectFormat gives the same answer for a file head and for raw bytes", "[FormatDetection]")
{
    // Same bytes must produce the same detection result whether the input is a path (file open + head read) or a raw
    // byte slice (network, stdin).
    const TempDir dir("format_detection");

    struct Case
    {
        const char *name;
        std::string_view bytes;
        loglib::LogFormat expected;
    };
    const Case cases[] = {
        {.name = "json.log",
         .bytes = R"({"timestamp":"2026-01-01T00:00:00Z","level":"info","message":"hi"}
{"timestamp":"2026-01-01T00:00:01Z","level":"warn","message":"there"}
)",
         .expected = loglib::LogFormat::Json},
        {.name = "logfmt.log",
         .bytes = R"(time=2026-01-01T00:00:00Z level=info message=hi
time=2026-01-01T00:00:01Z level=warn message=there
)",
         .expected = loglib::LogFormat::Logfmt},
        {.name = "csv.log",
         .bytes = "timestamp,level,message\n2026-01-01T00:00:00Z,info,hi\n2026-01-01T00:00:01Z,warn,there\n",
         .expected = loglib::LogFormat::Csv},
    };

    for (const Case &c : cases)
    {
        CAPTURE(c.name);
        const FormatSpec byPath = DetectFile(dir.Write(c.name, c.bytes));
        const std::optional<FormatSpec> byBytes = DetectFormat(c.bytes);
        REQUIRE(byBytes.has_value());
        CHECK(byPath.format == c.expected);
        CHECK(byBytes->format == c.expected);
        CHECK(byPath.regex.pattern == byBytes->regex.pattern);
    }
}

TEST_CASE("DetectFormat agrees on regex-format bytes and pattern", "[FormatDetection]")
{
    const TempDir dir("format_detection_regex");

    struct Case
    {
        const char *name;
        std::string_view bytes;
    };
    const Case cases[] = {
        {.name = "syslog_rfc3164.log",
         .bytes = "Apr 28 04:02:03 host-a systemd: System starting\n"
                  "Jun 27 01:47:20 host-b configd[17]: network changed\n"},
        {.name = "apache_common.log",
         .bytes = R"(127.0.0.1 - frank [10/Oct/2000:13:55:36 -0700] "GET /apache_pb.gif HTTP/1.0" 200 2326)"
                  "\n"
                  R"(10.1.10.51 - - [23/Dec/2014:21:20:35 +0000] "POST /api/1/rest/foo HTTP/1.1" 200 -)"
                  "\n"},
    };

    for (const Case &c : cases)
    {
        CAPTURE(c.name);
        const FormatSpec byPath = DetectFile(dir.Write(c.name, c.bytes));
        const std::optional<FormatSpec> byBytes = DetectFormat(c.bytes);
        REQUIRE(byBytes.has_value());
        CHECK(byPath.format == loglib::LogFormat::Regex);
        CHECK(byBytes->format == loglib::LogFormat::Regex);
        CHECK_FALSE(byBytes->regex.pattern.empty());
        CHECK(byPath.regex.pattern == byBytes->regex.pattern);
    }
}

TEST_CASE(
    "DetectFormat prefers a matching regex template over CSV when a comma-decimal timestamp would false-positive as "
    "CSV",
    "[FormatDetection]"
)
{
    // Java Logback / log4j2 / SLF4J default PatternLayout emits `2026-01-01 12:00:00,123 LEVEL ...` where the sole
    // comma is the millisecond decimal. The CSV check accepts any input with a consistent 2-cell-per-line comma count,
    // so without the regex-templates-before-CSV order this stream would be misparsed as a 2-column CSV. The shipped
    // "Java / log4j / SLF4J Logback" template matches these lines, so detection must land on Regex.
    constexpr std::string_view JAVA_STREAM =
        "2026-01-01 12:00:00,123 INFO  [main] com.example.App - Application starting\n"
        "2026-01-01 12:00:00,456 WARN  [http-nio-8080-exec-1] o.s.web.servlet.PageNotFound - No mapping\n"
        "2026-01-01 12:00:00,789 ERROR [pool-1-thread-3] com.example.Worker$Inner - Task failed\n";

    const std::optional<FormatSpec> detected = DetectFormat(JAVA_STREAM);
    REQUIRE(detected.has_value());
    CHECK(detected->format == loglib::LogFormat::Regex);
    CHECK_FALSE(detected->regex.pattern.empty());
}

TEST_CASE("ReadProbeHead on a missing file is empty", "[FormatDetection]")
{
    const std::filesystem::path missing = "definitely_not_a_real_file_1741d61e.log";
    REQUIRE_FALSE(std::filesystem::exists(missing));

    CHECK(ReadProbeHead(missing).empty());
    CHECK_FALSE(DetectFormat(ReadProbeHead(missing)).has_value());
}

TEST_CASE("DetectFormat returns nothing for an empty sample and for unrecognized content", "[FormatDetection]")
{
    CHECK_FALSE(DetectFormat({}).has_value());
    CHECK_FALSE(DetectFormat("just some words\nthat nothing recognizes\n").has_value());
    // The default-constructed FormatSpec is JSON, which is what callers that always need a parser fall back to.
    CHECK(FormatSpec{}.format == loglib::LogFormat::Json);
}

TEST_CASE("Two catalogs with different extras detect differently in the same process", "[FormatDetection]")
{
    RegexTemplate custom;
    custom.name = "Custom";
    custom.regex.pattern = R"(^(?<Stamp>\d{4}) \| (?<Text>.+)$)";
    const std::string_view sample = "2026 | first line\n2027 | second line\n";

    const RegexTemplateCatalog withExtra = RegexTemplateCatalog::Builtin().WithExtras({custom});
    const std::optional<FormatSpec> viaExtra = DetectFormat(sample, withExtra);
    REQUIRE(viaExtra.has_value());
    CHECK(viaExtra->format == loglib::LogFormat::Regex);
    CHECK(viaExtra->regex.pattern == custom.regex.pattern);

    // The built-in catalog does not know the template, so it does not claim the sample as that pattern.
    const std::optional<FormatSpec> viaBuiltin = DetectFormat(sample);
    if (viaBuiltin.has_value())
    {
        CHECK(viaBuiltin->regex.pattern != custom.regex.pattern);
    }
}

TEST_CASE("SampleFitsFormat checks the pinned pattern and not the catalog", "[FormatDetection]")
{
    SECTION("A matching pattern is accepted even when no template matches")
    {
        const std::string_view sample = "2026 | first line\n2027 | second line\n";
        REQUIRE_FALSE(DetectFormat(sample).has_value());
        const FormatSpec spec{
            .format = loglib::LogFormat::Regex, .regex = {.pattern = R"(^(?<Stamp>\d{4}) \| (?<Text>.+)$)"}
        };
        CHECK(SampleFitsFormat(sample, spec));
    }

    SECTION("A pattern that does not match is rejected even when a template matches")
    {
        const std::string_view sample = "Apr 28 04:02:03 host-a systemd: System starting\n"
                                        "Jun 27 01:47:20 host-b configd[17]: network changed\n";
        REQUIRE(DetectFormat(sample).has_value());
        const FormatSpec spec{
            .format = loglib::LogFormat::Regex, .regex = {.pattern = R"(^(?<Stamp>\d{4}) \| (?<Text>.+)$)"}
        };
        CHECK_FALSE(SampleFitsFormat(sample, spec));
    }

    SECTION("An empty pattern and a broken pattern do not fit")
    {
        const std::string_view sample = "a\nb\n";
        CHECK_FALSE(SampleFitsFormat(sample, FormatSpec{.format = loglib::LogFormat::Regex}));
        CHECK_FALSE(
            SampleFitsFormat(sample, FormatSpec{.format = loglib::LogFormat::Regex, .regex = {.pattern = "(?<A>("}})
        );
    }

    SECTION("The other formats use their own check")
    {
        CHECK(SampleFitsFormat(R"({"a":1})", FormatSpec{.format = loglib::LogFormat::Json}));
        CHECK_FALSE(SampleFitsFormat("a=1 b=2\n", FormatSpec{.format = loglib::LogFormat::Json}));
        CHECK(SampleFitsFormat("a=1 b=2\nc=3 d=4\n", FormatSpec{.format = loglib::LogFormat::Logfmt}));
        CHECK(SampleFitsFormat("a,b\n1,2\n3,4\n", FormatSpec{.format = loglib::LogFormat::Csv}));
    }
}

TEST_CASE("MakeParser returns a usable parser for every format", "[FormatDetection]")
{
    for (const loglib::LogFormat format :
         {loglib::LogFormat::Json, loglib::LogFormat::Logfmt, loglib::LogFormat::Csv, loglib::LogFormat::Regex})
    {
        CAPTURE(static_cast<int>(format));
        CHECK(MakeParser(FormatSpec{.format = format}) != nullptr);
    }
}

TEST_CASE("FormatSpec compares by format and settings", "[FormatDetection]")
{
    const FormatSpec json;
    CHECK(json.format == loglib::LogFormat::Json);
    CHECK(json.logfmtMultiline);
    CHECK(json == FormatSpec{});

    FormatSpec regex{.format = loglib::LogFormat::Regex, .regex = {.pattern = "^(?<a>.*)$"}};
    CHECK(regex != json);
    FormatSpec other = regex;
    CHECK(other == regex);
    other.regex.continuation = loglib::ContinuationMode::Indented;
    CHECK(other != regex);
}
