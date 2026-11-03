
#include "common.hpp"

#include <loglib/parse/parse_file.hpp>

#include <catch2/catch_all.hpp>
#include <glaze/glaze.hpp>

#include <string_view>
#include <utility>
#include <vector>

using namespace loglib;

TEST_CASE("Parse JSON log file via ParseFile auto-detect", "[parse_file]")
{
    std::vector<test_common::LogRecord> records = {
        glz::generic_sorted_u64{{"key", "value"}},
    };
    const TestStructuredLogFile testFile(std::move(records), test_common::JsonLines());

    ParseResult result = ParseFile(testFile.GetFilePath());
    CHECK(result.errors.empty());
    CHECK(result.data.Lines().size() == 1);
}

TEST_CASE("Parse logfmt log file via ParseFile auto-detect", "[parse_file]")
{
    const TestLogFile testFile("autodetect.logfmt");
    testFile.Write("level=info msg=\"hello\"\n");

    ParseResult result = ParseFile(testFile.GetFilePath());
    CHECK(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 1);
    CHECK(AsStringView(result.data.Lines()[0].GetValue("msg")) == std::string_view{"hello"});
}

TEST_CASE("Parse CSV log file via ParseFile auto-detect", "[parse_file]")
{
    const TestLogFile testFile("autodetect.csv");
    testFile.Write("level,msg\ninfo,hello\n");

    ParseResult result = ParseFile(testFile.GetFilePath());
    CHECK(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 1);
    CHECK(AsStringView(result.data.Lines()[0].GetValue("msg")) == std::string_view{"hello"});
}

TEST_CASE("Parse regex log file via ParseFile auto-detect", "[parse_file]")
{
    // Detection must build a regex parser pinned to the matched
    // template's pattern, not an unpinned one that would surface the
    // "empty pattern" error. The syslog fixture picks up the shipped
    // RFC3164 template.
    const TestLogFile testFile("autodetect.syslog");
    testFile.Write(
        "Apr 28 04:02:03 host-a systemd: System starting\n"
        "Apr 28 04:02:04 host-a systemd: another line\n"
    );

    ParseResult result = ParseFile(testFile.GetFilePath());
    CHECK(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 2);
    CHECK(AsStringView(result.data.Lines()[0].GetValue("hostname")) == std::string_view{"host-a"});
    CHECK(AsStringView(result.data.Lines()[0].GetValue("program")) == std::string_view{"systemd"});
}

TEST_CASE("ParseFile auto-detect rejects nonexistent or invalid file", "[parse_file]")
{
    CHECK_THROWS_AS(ParseFile("nonexistent.json"), std::runtime_error);

    // "Invalid log line." has no '=' and no ',' so none of the JSON,
    // logfmt, or CSV probes accept it. Two lines so the regex
    // probe's `IS_VALID_MIN_MATCHES = 2` threshold has a fair
    // chance to trigger, and neither line matches any built-in
    // regex template — so the final `throw runtime_error` at the
    // bottom of the sweep fires cleanly.
    const TestLogFile testFile;
    testFile.Write("Invalid log line.\nAnother nonsense line.\n");
    CHECK_THROWS_AS(ParseFile(testFile.GetFilePath()), std::runtime_error);
}
