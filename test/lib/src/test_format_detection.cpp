#include <loglib/format_detection.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_parser.hpp>

#include <test_common/temp_dir.hpp>

#include <catch2/catch_all.hpp>

#include <filesystem>
#include <string>
#include <string_view>

using loglib::DetectedFormat;
using loglib::DetectFormatForPath;
using loglib::DetectFormatFromBytes;
using loglib::LogConfiguration;
using loglib::MakeParserForFormat;
using test_common::TempDir;

TEST_CASE("DetectFormatFromBytes parity with DetectFormatForPath", "[FormatDetection]")
{
    // Same bytes must produce the same detection result whether
    // the input is a path (file open + head read) or a raw byte
    // slice (network, stdin). The parity test is the whole point
    // of hoisting the detector into loglib.
    const TempDir dir("format_detection");

    struct Case
    {
        const char *name;
        std::string_view bytes;
        LogConfiguration::Source::Format expected;
    };
    const Case cases[] = {
        {"json.log",
         R"({"timestamp":"2026-01-01T00:00:00Z","level":"info","message":"hi"}
{"timestamp":"2026-01-01T00:00:01Z","level":"warn","message":"there"}
)",
         LogConfiguration::Source::Format::Json},
        {"logfmt.log",
         R"(time=2026-01-01T00:00:00Z level=info message=hi
time=2026-01-01T00:00:01Z level=warn message=there
)",
         LogConfiguration::Source::Format::Logfmt},
        {"csv.log",
         "timestamp,level,message\n2026-01-01T00:00:00Z,info,hi\n2026-01-01T00:00:01Z,warn,there\n",
         LogConfiguration::Source::Format::Csv},
    };

    for (const Case &c : cases)
    {
        CAPTURE(c.name);
        const DetectedFormat byPath = DetectFormatForPath(dir.Write(c.name, c.bytes));
        const DetectedFormat byBytes = DetectFormatFromBytes(c.bytes);
        CHECK(byPath.format == c.expected);
        CHECK(byBytes.format == c.expected);
        CHECK(byPath.regexPattern == byBytes.regexPattern);
    }
}

TEST_CASE(
    "DetectFormatFromBytes and DetectFormatForPath agree on regex-format bytes and pattern", "[FormatDetection]"
)
{
    // Regression guard for the regex branch: both entry points must
    // report `Format::Regex` *and* the exact same `regexPattern`
    // (the resolved built-in template's compiled pattern), so a
    // stdin/network-stream open produces the same on-disk source
    // stanza as a file open of the same bytes.
    const TempDir dir("format_detection_regex");

    struct Case
    {
        const char *name;
        std::string_view bytes;
    };
    const Case cases[] = {
        {"syslog_rfc3164.log",
         "Apr 28 04:02:03 host-a systemd: System starting\n"
         "Jun 27 01:47:20 host-b configd[17]: network changed\n"},
        {"apache_common.log",
         R"(127.0.0.1 - frank [10/Oct/2000:13:55:36 -0700] "GET /apache_pb.gif HTTP/1.0" 200 2326)"
         "\n"
         R"(10.1.10.51 - - [23/Dec/2014:21:20:35 +0000] "POST /api/1/rest/foo HTTP/1.1" 200 -)"
         "\n"},
    };

    for (const Case &c : cases)
    {
        CAPTURE(c.name);
        const DetectedFormat byPath = DetectFormatForPath(dir.Write(c.name, c.bytes));
        const DetectedFormat byBytes = DetectFormatFromBytes(c.bytes);
        CHECK(byPath.format == LogConfiguration::Source::Format::Regex);
        CHECK(byBytes.format == LogConfiguration::Source::Format::Regex);
        CHECK_FALSE(byBytes.regexPattern.empty());
        CHECK(byPath.regexPattern == byBytes.regexPattern);
    }
}

TEST_CASE(
    "DetectFormatFromBytes prefers a matching regex template over CSV when a comma-decimal timestamp would "
    "false-positive as CSV",
    "[FormatDetection]"
)
{
    // Java/Logback / log4j2 / SLF4J default PatternLayout emits
    // `2026-01-01 12:00:00,123 LEVEL ...` where the sole comma is
    // the millisecond decimal. `CsvParser::IsValidBytes` accepts
    // any input with a consistent 2-cell-per-line comma count, so
    // without the regex-templates-before-CSV probe order this
    // Java stream was silently misparsed as a 2-column CSV --
    // splitting each row at the `,SSS` boundary and lumping level,
    // thread, logger, and message into the second cell.
    //
    // Regression guard for the fix: the shipped
    // "Java / log4j / SLF4J Logback" template matches these
    // lines, so the detector must land on `Format::Regex` with a
    // non-empty pattern, not on `Format::Csv`.
    constexpr std::string_view JAVA_STREAM =
        "2026-01-01 12:00:00,123 INFO  [main] com.example.App - Application starting\n"
        "2026-01-01 12:00:00,456 WARN  [http-nio-8080-exec-1] o.s.web.servlet.PageNotFound - No mapping\n"
        "2026-01-01 12:00:00,789 ERROR [pool-1-thread-3] com.example.Worker$Inner - Task failed\n";

    const DetectedFormat detected = DetectFormatFromBytes(JAVA_STREAM);
    CHECK(detected.format == LogConfiguration::Source::Format::Regex);
    CHECK_FALSE(detected.regexPattern.empty());
}

TEST_CASE("DetectFormatForPath on a missing file falls back to default Json", "[FormatDetection]")
{
    // `ReadProbeHead` returns an empty string when the file cannot
    // be opened; `DetectFormatFromBytes({})` then hands back the
    // default `Format::Json` verdict with an empty pattern. The
    // header contract at `format_detection.hpp` promises this
    // exact fallback -- stdin / network-stream callers rely on it
    // to keep spinning a parser instead of dead-ending.
    const std::filesystem::path missing = "definitely_not_a_real_file_1741d61e.log";
    REQUIRE_FALSE(std::filesystem::exists(missing));

    const DetectedFormat detected = DetectFormatForPath(missing);
    CHECK(detected.format == LogConfiguration::Source::Format::Json);
    CHECK(detected.regexPattern.empty());
}

TEST_CASE("DetectFormatFromBytes on empty buffer yields default Json", "[FormatDetection]")
{
    // Empty buffer -> nothing to sniff. The default-constructed
    // `DetectedFormat` deliberately defaults to `Json` so that the
    // rest of the streaming pipeline (which cannot early-out on
    // "unknown format") still spins up a usable parser.
    const DetectedFormat detected = DetectFormatFromBytes({});
    CHECK(detected.format == LogConfiguration::Source::Format::Json);
    CHECK(detected.regexPattern.empty());
}

TEST_CASE("MakeParserForFormat returns a usable parser for every format", "[FormatDetection]")
{
    // Regression: renaming `IsValid(path)` to `IsValidBytes(view)`
    // must not break the plain factory dispatch. Passing a
    // buffer that matches each format proves the returned parser
    // isn't a stub.
    struct Case
    {
        LogConfiguration::Source::Format format;
        std::string_view probe;
    };
    const Case cases[] = {
        {LogConfiguration::Source::Format::Json,
         R"({"a":1}
{"b":2}
)"},
        {LogConfiguration::Source::Format::Logfmt, "a=1 b=2\nc=3 d=4\n"},
        {LogConfiguration::Source::Format::Csv, "a,b\n1,2\n3,4\n"},
    };

    for (const Case &c : cases)
    {
        CAPTURE(static_cast<int>(c.format));
        const auto parser = MakeParserForFormat(c.format);
        REQUIRE(parser != nullptr);
        CHECK(parser->IsValidBytes(c.probe));
    }
}
