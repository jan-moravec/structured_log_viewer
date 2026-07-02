// Round-trip coverage for the per-template `test_common::LogFormat`
// synthesizers. Each `TEST_CASE` drives 1000 random `LogRecord`s
// through one synthesizer, then parses the resulting file with the
// matching shipped `loglib::RegexTemplate` via `RegexParser`. The
// assertion is tight: every line must produce exactly one row and
// zero parse errors.
//
// Load-bearing regression guard for synthesizer/pattern drift.
// `test_regex_templates.cpp` only asserts the 3-5 hand-curated
// `sampleLines` per template round-trip; the RNG-driven fixture
// here exercises the full match surface (rare pool combinations,
// edge-case message contents, `[pid]`-present and `[pid]`-absent
// syslog shapes, ...) so a future pattern tightening that would
// break synthetic-but-valid lines fails here rather than silently
// in the wild.

#include "common.hpp"

#include <loglib/log_data.hpp>
#include <loglib/parse_file.hpp>
#include <loglib/parsers/regex_parser.hpp>
#include <loglib/regex_templates.hpp>

#include <test_common/log_format.hpp>
#include <test_common/log_generator.hpp>
#include <test_common/log_record.hpp>

#include <catch2/catch_all.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using namespace loglib;

namespace
{

/// Serialize @p records through @p format into @p filePath, run
/// `RegexParser(pattern)` over the file, and CHECK every line
/// parsed cleanly. Kept as a helper so the per-template `TEST_CASE`s
/// stay one-liners and any future assertion change (column spot-
/// check, timing budget, ...) lands in one place.
void RoundTripThroughTemplate(
    const std::vector<test_common::LogRecord> &records,
    const test_common::LogFormat &format,
    const std::string &templateName,
    const std::string &filePath
)
{
    const test_common::RecordSchema emptySchema;
    const TestStructuredLogFile fixture(records, format, emptySchema, filePath);

    const RegexTemplate *tmpl = FindTemplateByName(templateName);
    REQUIRE(tmpl != nullptr);

    const RegexParser parser{tmpl->pattern};
    const ParseResult result = ParseFile(parser, fixture.GetFilePath());

    // A single parse error surfaces the offending line number, so
    // the check-then-require pattern gives us the most useful
    // failure message when a regression lands.
    CHECK(result.errors.empty());
    if (!result.errors.empty())
    {
        FAIL("First error: " << result.errors.front());
    }
    REQUIRE(result.data.Lines().size() == records.size());
}

constexpr std::uint32_t GENERATOR_TEST_SEED = 0xA11CE;
constexpr std::size_t GENERATOR_TEST_LINES = 1000;

} // namespace

TEST_CASE(
    "Synthesized syslog lines parse under Syslog (RFC3164) [regex_templates]", "[regex_templates][regex_parser]"
)
{
    const auto records = test_common::GenerateRandomLogRecords(GENERATOR_TEST_LINES, GENERATOR_TEST_SEED);
    RoundTripThroughTemplate(records, test_common::SyslogRfc3164Format(), "Syslog (RFC3164)", "regex_gen_syslog.log");
}

TEST_CASE(
    "Synthesized Apache Combined lines parse under Apache/nginx Combined Log Format [regex_templates]",
    "[regex_templates][regex_parser]"
)
{
    const auto records = test_common::GenerateRandomLogRecords(GENERATOR_TEST_LINES, GENERATOR_TEST_SEED);
    RoundTripThroughTemplate(
        records,
        test_common::ApacheCombinedFormat(),
        "Apache/nginx Combined Log Format",
        "regex_gen_apache_combined.log"
    );
}

TEST_CASE(
    "Synthesized Apache Common lines parse under Apache/nginx Common Log Format [regex_templates]",
    "[regex_templates][regex_parser]"
)
{
    const auto records = test_common::GenerateRandomLogRecords(GENERATOR_TEST_LINES, GENERATOR_TEST_SEED);
    RoundTripThroughTemplate(
        records,
        test_common::ApacheCommonFormat(),
        "Apache/nginx Common Log Format",
        "regex_gen_apache_common.log"
    );
}

TEST_CASE(
    "Synthesized Apache error lines parse under Apache error log [regex_templates]",
    "[regex_templates][regex_parser]"
)
{
    const auto records = test_common::GenerateRandomLogRecords(GENERATOR_TEST_LINES, GENERATOR_TEST_SEED);
    RoundTripThroughTemplate(
        records, test_common::ApacheErrorFormat(), "Apache error log", "regex_gen_apache_error.log"
    );
}

TEST_CASE(
    "Synthesized Java log lines parse under Java / log4j / SLF4J Logback [regex_templates]",
    "[regex_templates][regex_parser]"
)
{
    const auto records = test_common::GenerateRandomLogRecords(GENERATOR_TEST_LINES, GENERATOR_TEST_SEED);
    RoundTripThroughTemplate(
        records, test_common::JavaLogFormat(), "Java / log4j / SLF4J Logback", "regex_gen_java_log.log"
    );
}
