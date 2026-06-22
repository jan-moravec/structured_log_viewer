#include "common.hpp"

#include <loglib/log_configuration.hpp>
#include <loglib/log_data.hpp>
#include <loglib/parse_file.hpp>
#include <loglib/parser_options.hpp>
#include <loglib/parsers/regex_parser.hpp>
#include <loglib/parsers/json_parser.hpp>
#include <loglib/regex_templates.hpp>

#include <catch2/catch_all.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <variant>

using namespace loglib;

TEST_CASE("RegexParser validates against built-in templates [regex]", "[regex_parser]")
{
    const RegexParser parser;
    const TestLogFile file("regex_isvalid.log");
    // Two syslog lines — enough to trip the IS_VALID_MIN_MATCHES = 2
    // guard and identify Syslog (RFC3164).
    file.Write("Apr 28 04:02:03 host-a systemd: System starting\n"
               "Jun 27 01:47:20 host-b configd[17]: network changed\n");
    CHECK(parser.IsValid(file.GetFilePath()));

    const RegexTemplate *detected = DetectRegexTemplate(file.GetFilePath());
    REQUIRE(detected != nullptr);
    CHECK(detected->name == "Syslog (RFC3164)");
}

TEST_CASE("RegexParser rejects single-line files [regex]", "[regex_parser]")
{
    // Pattern-matching one line is too brittle for auto-detect; the
    // probe requires at least two non-blank probe lines.
    const RegexParser parser;
    const TestLogFile file("regex_oneline.log");
    file.Write("Apr 28 04:02:03 host-a systemd: System starting\n");
    CHECK_FALSE(parser.IsValid(file.GetFilePath()));
}

TEST_CASE("RegexParser rejects JSON / logfmt files [regex]", "[regex_parser]")
{
    // Auto-detect precedence demonstration: a file that the other
    // probes claim must not also be claimed by Regex. Run on JSON
    // because it's the broadest non-regex shape.
    const RegexParser parser;
    const TestLogFile file("regex_rejects_json.log");
    file.Write(R"({"level":"info","msg":"hello"})"
               "\n"
               R"({"level":"warn","msg":"world"})"
               "\n");
    CHECK_FALSE(parser.IsValid(file.GetFilePath()));
}

TEST_CASE("RegexParser default-constructed parse without pattern surfaces error [regex]", "[regex_parser]")
{
    // `LogFactory::Create(Regex)` returns a no-pattern instance.
    // Calling parse on it must not crash; it must surface one error
    // and end the parse cleanly.
    const RegexParser parser;
    const TestLogFile file("regex_no_pattern.log");
    file.Write("Apr 28 04:02:03 host-a systemd: System starting\n"
               "Apr 28 04:02:04 host-a systemd: another line\n");

    auto result = ParseFile(parser, file.GetFilePath());
    REQUIRE(result.errors.size() == 1);
    CHECK(result.errors[0].find("non-empty pattern") != std::string::npos);
    CHECK(result.data.Lines().empty());
}

TEST_CASE("RegexParser unparseable pattern surfaces error [regex]", "[regex_parser]")
{
    const RegexParser parser(R"((?<a)"); // dangling group
    const TestLogFile file("regex_bad_pattern.log");
    file.Write("anything\nat all\n");

    auto result = ParseFile(parser, file.GetFilePath());
    REQUIRE(result.errors.size() == 1);
    CHECK(result.errors[0].find("Pattern compile failed") != std::string::npos);
    CHECK(result.data.Lines().empty());
}

TEST_CASE("RegexParser pattern without named groups surfaces error [regex]", "[regex_parser]")
{
    // Anonymous groups don't map to columns; the parser refuses
    // rather than producing schemaless rows.
    const RegexParser parser(R"(^(\w+)\s+(.*)$)");
    const TestLogFile file("regex_no_groups.log");
    file.Write("a b\nc d\n");

    auto result = ParseFile(parser, file.GetFilePath());
    REQUIRE(result.errors.size() == 1);
    CHECK(result.errors[0].find("named capture groups") != std::string::npos);
    CHECK(result.data.Lines().empty());
}

TEST_CASE("RegexParser parses well-formed lines [regex]", "[regex_parser]")
{
    // Simple `LEVEL message` shape; named groups -> columns.
    const RegexParser parser(R"(^(?<level>\w+)\s+(?<message>.*)$)");
    const TestLogFile file("regex_parse_minimal.log");
    file.Write("info hello\nerror boom\n");

    auto result = ParseFile(parser, file.GetFilePath());
    CHECK(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 2);
    CHECK(AsStringView(result.data.Lines()[0].GetValue("level")) == std::string_view{"info"});
    CHECK(AsStringView(result.data.Lines()[0].GetValue("message")) == std::string_view{"hello"});
    CHECK(AsStringView(result.data.Lines()[1].GetValue("level")) == std::string_view{"error"});
    CHECK(AsStringView(result.data.Lines()[1].GetValue("message")) == std::string_view{"boom"});
}

TEST_CASE("RegexParser types numeric captures [regex]", "[regex_parser]")
{
    // ClassifyBareScalar promotes numeric / bool captures the same
    // way it does for CSV / logfmt bare cells.
    const RegexParser parser(R"(^(?<level>\w+)\s+(?<code>\d+)\s+(?<ratio>\S+)\s+(?<ok>\S+)$)");
    const TestLogFile file("regex_typing.log");
    file.Write("info 200 0.75 true\n"
               "warn 404 -1.5 false\n");

    auto result = ParseFile(parser, file.GetFilePath());
    REQUIRE(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 2);

    const auto &row0 = result.data.Lines()[0];
    CHECK(std::get<std::uint64_t>(row0.GetValue("code")) == 200U);
    CHECK(std::get<double>(row0.GetValue("ratio")) == Catch::Approx(0.75));
    CHECK(std::get<bool>(row0.GetValue("ok")) == true);

    const auto &row1 = result.data.Lines()[1];
    CHECK(std::get<std::uint64_t>(row1.GetValue("code")) == 404U);
    CHECK(std::get<double>(row1.GetValue("ratio")) == Catch::Approx(-1.5));
    CHECK(std::get<bool>(row1.GetValue("ok")) == false);
}

TEST_CASE("RegexParser non-matching lines surface as errors [regex]", "[regex_parser]")
{
    // Non-match is per-line; the rest of the file still parses.
    const RegexParser parser(R"(^(?<level>\w+):(?<message>.+)$)");
    const TestLogFile file("regex_non_matching.log");
    file.Write("info:hello\n"
               "this line does not match\n"
               "error:boom\n");

    auto result = ParseFile(parser, file.GetFilePath());
    REQUIRE(result.data.Lines().size() == 2);
    REQUIRE(result.errors.size() == 1);
    CHECK(result.errors[0].find("line 2") != std::string::npos);
    CHECK(result.errors[0].find("did not match") != std::string::npos);
}

TEST_CASE("RegexParser optional unmatched groups -> monostate [regex]", "[regex_parser]")
{
    // `pid` is optional; absent on the first line, present on the
    // second. Captures that didn't participate in the match drop to
    // monostate (not the empty string) so column lookups behave
    // like CSV's "missing trailing cell" case.
    const RegexParser parser(R"(^(?<program>\w+)(?:\[(?<pid>\d+)\])?:\s+(?<message>.*)$)");
    const TestLogFile file("regex_optional.log");
    file.Write("systemd: System starting\n"
               "configd[17]: network changed\n");

    auto result = ParseFile(parser, file.GetFilePath());
    REQUIRE(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 2);

    const auto &row0 = result.data.Lines()[0];
    CHECK(AsStringView(row0.GetValue("program")) == std::string_view{"systemd"});
    CHECK(std::holds_alternative<std::monostate>(row0.GetValue("pid")));
    CHECK_FALSE(row0.Values().contains("pid"));

    const auto &row1 = result.data.Lines()[1];
    CHECK(AsStringView(row1.GetValue("program")) == std::string_view{"configd"});
    CHECK(std::get<std::uint64_t>(row1.GetValue("pid")) == 17U);
}

TEST_CASE("RegexParser auto-detect through ParseFile picks the matched template [regex]", "[regex_parser]")
{
    // End-to-end: `loglib::ParseFile(path)` runs the full auto-detect
    // loop including the Regex special-case branch. The file has no
    // header, just two syslog-shaped lines.
    const TestLogFile file("regex_e2e.log");
    file.Write("Apr 28 04:02:03 host-a systemd: System starting\n"
               "Apr 28 04:02:04 host-b CRON[1234]: (root) CMD (test)\n");

    auto result = ParseFile(file.GetFilePath());
    CHECK(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 2);
    // Columns from the syslog template.
    CHECK(AsStringView(result.data.Lines()[0].GetValue("hostname")) == std::string_view{"host-a"});
    CHECK(AsStringView(result.data.Lines()[0].GetValue("program")) == std::string_view{"systemd"});
    CHECK(AsStringView(result.data.Lines()[1].GetValue("hostname")) == std::string_view{"host-b"});
    CHECK(AsStringView(result.data.Lines()[1].GetValue("program")) == std::string_view{"CRON"});
    CHECK(std::get<std::uint64_t>(result.data.Lines()[1].GetValue("pid")) == 1234U);
}

TEST_CASE("RegexParser does not steal JSON / CSV files [regex]", "[regex_parser]")
{
    // Regression: the auto-detect order is JSON to logfmt to CSV to
    // Regex. A two-line JSON file must come out as JSON; the regex
    // probe is the *last* fallback, so we'd see syslog-style columns
    // if the order ever drifted.
    const TestLogFile file("regex_precedence.log");
    file.Write(R"({"level":"info","msg":"hello"})"
               "\n"
               R"({"level":"warn","msg":"world"})"
               "\n");

    auto result = ParseFile(file.GetFilePath());
    CHECK(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 2);
    // If Regex had won, we'd not see a `level` JSON column.
    CHECK(AsStringView(result.data.Lines()[0].GetValue("level")) == std::string_view{"info"});
}

TEST_CASE("RegexParser ToString joins values in KeyId order [regex]", "[regex_parser]")
{
    // Best-effort round-trip: regex is not invertible, so we accept
    // any space-separated form that includes all the captured
    // values. Used only when the line's source bytes are gone.
    const RegexParser parser(R"(^(?<level>\w+)\s+(?<message>.*)$)");
    const TestLogFile file("regex_tostring.log");
    file.Write("info hello world\n");

    auto result = ParseFile(parser, file.GetFilePath());
    REQUIRE(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 1);

    const std::string out = parser.ToString(result.data.Lines()[0]);
    CHECK(out.find("info") != std::string::npos);
    CHECK(out.find("hello world") != std::string::npos);
}
