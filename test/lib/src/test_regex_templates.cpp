#include "common.hpp"

#include <loglib/log_data.hpp>
#include <loglib/parse_file.hpp>
#include <loglib/parsers/regex_parser.hpp>
#include <loglib/regex_templates.hpp>

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <span>
#include <string>
#include <string_view>

using namespace loglib;

namespace
{

/// Slugify @p name into something safe to use as a file basename:
/// the built-in template names contain `/` and other punctuation
/// that would be interpreted as path separators by `TestLogFile`.
std::string SlugifyName(std::string_view name)
{
    std::string slug{name};
    std::ranges::replace_if(
        slug,
        [](char c) { return !static_cast<bool>(std::isalnum(static_cast<unsigned char>(c))); },
        '_'
    );
    return slug;
}

/// Drive @p pattern through `RegexParser` over a synthetic file
/// containing @p lines. Wraps the static pipeline plus
/// `LogFactory`/`ParseFile`-style sink so we can assert on row /
/// error counts directly. Returns the `ParseResult` for further
/// inspection.
ParseResult ParseLinesWith(std::string_view pattern, std::span<const std::string> lines, std::string_view filePath)
{
    std::string content;
    for (const std::string &line : lines)
    {
        content.append(line);
        content.push_back('\n');
    }
    const TestLogFile file{std::string(filePath)};
    file.Write(content);

    const RegexParser parser{std::string(pattern)};
    return ParseFile(parser, file.GetFilePath());
}

} // namespace

TEST_CASE("Built-in regex templates compile and parse their sample lines [regex_templates]", "[regex_templates]")
{
    // For every built-in entry: compile the pattern and assert that
    // every sample line emits exactly one row (no errors). This is
    // both a syntax check (PCRE2 wouldn't compile a malformed
    // pattern) and a behavioural check that the named groups are
    // wired the way the template documents.
    const auto builtins = BuiltinRegexTemplates();
    REQUIRE_FALSE(builtins.empty());

    for (const RegexTemplate &t : builtins)
    {
        INFO("template: " << t.name);
        REQUIRE_FALSE(t.sampleLines.empty());

        // ParseFile needs at least two non-blank lines or the regex
        // auto-detect probe declines; we drive the pinned-pattern
        // RegexParser directly so single-sample templates still parse.
        const ParseResult result = ParseLinesWith(
            t.pattern, std::span<const std::string>(t.sampleLines), "regex_templates_" + SlugifyName(t.name) + ".log"
        );
        CHECK(result.errors.empty());
        CHECK(result.data.Lines().size() == t.sampleLines.size());
    }
}

TEST_CASE("DetectRegexTemplate identifies syslog samples [regex_templates]", "[regex_templates]")
{
    // Cross-template check: the syslog samples are *not* misidentified
    // as one of the other formats. The probe scans templates in
    // registry order; the first hit wins.
    const TestLogFile file{"regex_templates_detect.log"};
    file.Write("Apr 28 04:02:03 host-a systemd: System starting\n"
               "Jun 27 01:47:20 host-b configd[17]: network changed\n");
    const RegexTemplate *detected = DetectRegexTemplate(file.GetFilePath());
    REQUIRE(detected != nullptr);
    CHECK(detected->name == "Syslog (RFC3164)");
}

TEST_CASE("DetectRegexTemplate identifies Apache CLF samples [regex_templates]", "[regex_templates]")
{
    const TestLogFile file{"regex_templates_detect_clf.log"};
    file.Write(R"(127.0.0.1 - frank [10/Oct/2000:13:55:36 -0700] "GET /apache_pb.gif HTTP/1.0" 200 2326)"
               "\n"
               R"(10.1.10.51 - - [23/Dec/2014:21:20:35 +0000] "POST /api/1/rest/foo HTTP/1.1" 200 -)"
               "\n");
    const RegexTemplate *detected = DetectRegexTemplate(file.GetFilePath());
    REQUIRE(detected != nullptr);
    CHECK(detected->name == "Apache/nginx Common Log Format");
}

TEST_CASE("FindBuiltinByPattern round-trips every registry entry [regex_templates]", "[regex_templates]")
{
    // The GUI / config persistence relies on this lookup to recover
    // the display name from a saved pattern. Regression-guard
    // against a future refactor that changes string identity.
    for (const RegexTemplate &t : BuiltinRegexTemplates())
    {
        INFO("template: " << t.name);
        const RegexTemplate *found = FindBuiltinByPattern(t.pattern);
        REQUIRE(found != nullptr);
        CHECK(found->name == t.name);
    }

    // Unknown pattern (user-supplied custom) returns nullptr.
    CHECK(FindBuiltinByPattern("definitely not a built-in pattern") == nullptr);
}
