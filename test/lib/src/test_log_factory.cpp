
#include "common.hpp"

#include <loglib/log_factory.hpp>
#include <loglib/parse_file.hpp>

#include <catch2/catch_all.hpp>
#include <glaze/glaze.hpp>

#include <string_view>
#include <utility>
#include <vector>

using namespace loglib;

TEST_CASE("Create JSON logs parser", "[log_factory]")
{
    auto parser = LogFactory::Create(LogFactory::Parser::Json);
    CHECK(parser != nullptr);
}

TEST_CASE("Create logfmt parser", "[log_factory]")
{
    auto parser = LogFactory::Create(LogFactory::Parser::Logfmt);
    CHECK(parser != nullptr);
}

TEST_CASE("Create CSV parser", "[log_factory]")
{
    auto parser = LogFactory::Create(LogFactory::Parser::Csv);
    CHECK(parser != nullptr);
}

TEST_CASE("Create regex parser", "[log_factory]")
{
    // The regex parser factory branch is separately exercised by
    // `ParseFile(path)`'s auto-detect loop, but the direct
    // `Create` call is what registers the parser type in the
    // enum sweep (see the file-scope comment on `Parser` about
    // append-only enum values). Guards against a future rework
    // that drops the branch and silently makes `Parser::Regex`
    // throw the default `Invalid parser` runtime error.
    auto parser = LogFactory::Create(LogFactory::Parser::Regex);
    CHECK(parser != nullptr);
}

TEST_CASE("Create non-existent parser", "[log_factory]")
{
    CHECK_THROWS_AS(LogFactory::Create(LogFactory::Parser::Count), std::runtime_error);
}

TEST_CASE("Parse JSON log file via ParseFile auto-detect", "[log_factory]")
{
    std::vector<test_common::LogRecord> records = {
        glz::generic_sorted_u64{{"key", "value"}},
    };
    const TestStructuredLogFile testFile(std::move(records), test_common::JsonLines());

    ParseResult result = ParseFile(testFile.GetFilePath());
    CHECK(result.errors.empty());
    CHECK(result.data.Lines().size() == 1);
}

TEST_CASE("Parse logfmt log file via ParseFile auto-detect", "[log_factory]")
{
    const TestLogFile testFile("autodetect.logfmt");
    testFile.Write("level=info msg=\"hello\"\n");

    ParseResult result = ParseFile(testFile.GetFilePath());
    CHECK(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 1);
    CHECK(AsStringView(result.data.Lines()[0].GetValue("msg")) == std::string_view{"hello"});
}

TEST_CASE("Parse CSV log file via ParseFile auto-detect", "[log_factory]")
{
    const TestLogFile testFile("autodetect.csv");
    testFile.Write("level,msg\ninfo,hello\n");

    ParseResult result = ParseFile(testFile.GetFilePath());
    CHECK(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 1);
    CHECK(AsStringView(result.data.Lines()[0].GetValue("msg")) == std::string_view{"hello"});
}

TEST_CASE("Parse regex log file via ParseFile auto-detect", "[log_factory]")
{
    // Regression guard for the `Parser::Regex` special-case in
    // `ParseFile(path)`: the enum sweep must not drive a
    // factory-built regex parser directly (that instance has no
    // pinned pattern and would surface the "empty pattern"
    // error). Instead the loop calls `DetectRegexTemplate` and
    // builds a parser pinned to the matched template's pattern.
    // Syslog fixture picks up the shipped RFC3164 template.
    const TestLogFile testFile("autodetect.syslog");
    testFile.Write("Apr 28 04:02:03 host-a systemd: System starting\n"
                   "Apr 28 04:02:04 host-a systemd: another line\n");

    ParseResult result = ParseFile(testFile.GetFilePath());
    CHECK(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 2);
    CHECK(AsStringView(result.data.Lines()[0].GetValue("hostname")) == std::string_view{"host-a"});
    CHECK(AsStringView(result.data.Lines()[0].GetValue("program")) == std::string_view{"systemd"});
}

TEST_CASE("ParseFile auto-detect rejects nonexistent or invalid file", "[log_factory]")
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
