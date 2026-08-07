#include <loglib/format_detection.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_parser.hpp>

#include <test_common/temp_dir.hpp>

#include <catch2/catch_all.hpp>

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
