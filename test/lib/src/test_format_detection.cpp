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

using loglib::DetectedFormat;
using loglib::DetectFormat;
using loglib::FormatOptions;
using loglib::MakeParserForFormat;
using loglib::ReadProbeHead;
using loglib::RegexTemplate;
using loglib::RegexTemplateCatalog;
using loglib::SampleFitsFormat;
using loglib::Source;
using test_common::TempDir;

namespace
{

DetectedFormat DetectFile(const std::filesystem::path &file)
{
    return DetectFormat(ReadProbeHead(file)).value_or(DetectedFormat{});
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
        Source::Format expected;
    };
    const Case cases[] = {
        {.name = "json.log",
         .bytes = R"({"timestamp":"2026-01-01T00:00:00Z","level":"info","message":"hi"}
{"timestamp":"2026-01-01T00:00:01Z","level":"warn","message":"there"}
)",
         .expected = Source::Format::Json},
        {.name = "logfmt.log",
         .bytes = R"(time=2026-01-01T00:00:00Z level=info message=hi
time=2026-01-01T00:00:01Z level=warn message=there
)",
         .expected = Source::Format::Logfmt},
        {.name = "csv.log",
         .bytes = "timestamp,level,message\n2026-01-01T00:00:00Z,info,hi\n2026-01-01T00:00:01Z,warn,there\n",
         .expected = Source::Format::Csv},
    };

    for (const Case &c : cases)
    {
        CAPTURE(c.name);
        const DetectedFormat byPath = DetectFile(dir.Write(c.name, c.bytes));
        const std::optional<DetectedFormat> byBytes = DetectFormat(c.bytes);
        REQUIRE(byBytes.has_value());
        CHECK(byPath.format == c.expected);
        CHECK(byBytes->format == c.expected);
        CHECK(byPath.regexPattern == byBytes->regexPattern);
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
        const DetectedFormat byPath = DetectFile(dir.Write(c.name, c.bytes));
        const std::optional<DetectedFormat> byBytes = DetectFormat(c.bytes);
        REQUIRE(byBytes.has_value());
        CHECK(byPath.format == Source::Format::Regex);
        CHECK(byBytes->format == Source::Format::Regex);
        CHECK_FALSE(byBytes->regexPattern.empty());
        CHECK(byPath.regexPattern == byBytes->regexPattern);
    }
}

TEST_CASE(
    "DetectFormat prefers a matching regex template over CSV when a comma-decimal timestamp would false-positive as CSV",
    "[FormatDetection]"
)
{
    // Java Logback / log4j2 / SLF4J default PatternLayout emits `2026-01-01 12:00:00,123 LEVEL ...` where the sole comma
    // is the millisecond decimal. The CSV check accepts any input with a consistent 2-cell-per-line comma count, so
    // without the regex-templates-before-CSV order this stream would be misparsed as a 2-column CSV. The shipped
    // "Java / log4j / SLF4J Logback" template matches these lines, so detection must land on Regex.
    constexpr std::string_view JAVA_STREAM =
        "2026-01-01 12:00:00,123 INFO  [main] com.example.App - Application starting\n"
        "2026-01-01 12:00:00,456 WARN  [http-nio-8080-exec-1] o.s.web.servlet.PageNotFound - No mapping\n"
        "2026-01-01 12:00:00,789 ERROR [pool-1-thread-3] com.example.Worker$Inner - Task failed\n";

    const std::optional<DetectedFormat> detected = DetectFormat(JAVA_STREAM);
    REQUIRE(detected.has_value());
    CHECK(detected->format == Source::Format::Regex);
    CHECK_FALSE(detected->regexPattern.empty());
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
    // The default-constructed DetectedFormat is JSON, which is what callers that always need a parser fall back to.
    CHECK(DetectedFormat{}.format == Source::Format::Json);
}

TEST_CASE("Two catalogs with different extras detect differently in the same process", "[FormatDetection]")
{
    RegexTemplate custom;
    custom.name = "Custom";
    custom.pattern = R"(^(?<Stamp>\d{4}) \| (?<Text>.+)$)";
    const std::string_view sample = "2026 | first line\n2027 | second line\n";

    const RegexTemplateCatalog withExtra = RegexTemplateCatalog::Builtin().WithExtras({custom});
    const std::optional<DetectedFormat> viaExtra = DetectFormat(sample, withExtra);
    REQUIRE(viaExtra.has_value());
    CHECK(viaExtra->format == Source::Format::Regex);
    CHECK(viaExtra->regexPattern == custom.pattern);

    // The built-in catalog does not know the template, so it does not claim the sample as that pattern.
    const std::optional<DetectedFormat> viaBuiltin = DetectFormat(sample);
    if (viaBuiltin.has_value())
    {
        CHECK(viaBuiltin->regexPattern != custom.pattern);
    }
}

TEST_CASE("SampleFitsFormat checks the pinned pattern and not the catalog", "[FormatDetection]")
{
    SECTION("A matching pattern is accepted even when no template matches")
    {
        const std::string_view sample = "2026 | first line\n2027 | second line\n";
        REQUIRE_FALSE(DetectFormat(sample).has_value());
        const FormatOptions options{.regexPattern = R"(^(?<Stamp>\d{4}) \| (?<Text>.+)$)"};
        CHECK(SampleFitsFormat(sample, Source::Format::Regex, options));
    }

    SECTION("A pattern that does not match is rejected even when a template matches")
    {
        const std::string_view sample = "Apr 28 04:02:03 host-a systemd: System starting\n"
                                        "Jun 27 01:47:20 host-b configd[17]: network changed\n";
        REQUIRE(DetectFormat(sample).has_value());
        const FormatOptions options{.regexPattern = R"(^(?<Stamp>\d{4}) \| (?<Text>.+)$)"};
        CHECK_FALSE(SampleFitsFormat(sample, Source::Format::Regex, options));
    }

    SECTION("An empty pattern and a broken pattern do not fit")
    {
        const std::string_view sample = "a\nb\n";
        CHECK_FALSE(SampleFitsFormat(sample, Source::Format::Regex, FormatOptions{}));
        CHECK_FALSE(SampleFitsFormat(sample, Source::Format::Regex, FormatOptions{.regexPattern = "(?<A>("}));
    }

    SECTION("The other formats use their own check")
    {
        CHECK(SampleFitsFormat(R"({"a":1})", Source::Format::Json));
        CHECK_FALSE(SampleFitsFormat("a=1 b=2\n", Source::Format::Json));
        CHECK(SampleFitsFormat("a=1 b=2\nc=3 d=4\n", Source::Format::Logfmt));
        CHECK(SampleFitsFormat("a,b\n1,2\n3,4\n", Source::Format::Csv));
    }
}

TEST_CASE("MakeParserForFormat returns a usable parser for every format", "[FormatDetection]")
{
    for (const Source::Format format :
         {Source::Format::Json, Source::Format::Logfmt, Source::Format::Csv, Source::Format::Regex})
    {
        CAPTURE(static_cast<int>(format));
        CHECK(MakeParserForFormat(format) != nullptr);
    }
}
