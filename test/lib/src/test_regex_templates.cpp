#include "common.hpp"

#include <loglib/log_data.hpp>
#include <loglib/parse_file.hpp>
#include <loglib/parsers/regex_parser.hpp>
#include <loglib/regex_templates.hpp>

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <limits>
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

TEST_CASE("Built-in regex templates are returned in priority-then-document order [regex_templates]", "[regex_templates]")
{
    // The probe loop scans templates in `priority` ascending,
    // so the curated order is load-bearing. Assert the relative
    // order of the four Apache-family templates plus the generic
    // fallback to lock the documented "Combined before Common
    // before Apache error before Generic" invariant.
    //
    // This is a structural assertion against `BuiltinRegexTemplates`
    // — we don't drive a fixture file because the relative order
    // is what matters; behavioural coverage of "the right template
    // wins for an ambiguous file" lives in the existing detect
    // cases.
    const auto builtins = BuiltinRegexTemplates();
    auto indexOf = [&](std::string_view name) -> size_t {
        for (size_t i = 0; i < builtins.size(); ++i)
        {
            if (builtins[i].name == name)
            {
                return i;
            }
        }
        return std::numeric_limits<size_t>::max();
    };

    const size_t combined = indexOf("Apache/nginx Combined Log Format");
    const size_t common = indexOf("Apache/nginx Common Log Format");
    const size_t apacheError = indexOf("Apache error log");
    const size_t generic = indexOf("Generic bracketed level");

    REQUIRE(combined != std::numeric_limits<size_t>::max());
    REQUIRE(common != std::numeric_limits<size_t>::max());
    REQUIRE(apacheError != std::numeric_limits<size_t>::max());
    REQUIRE(generic != std::numeric_limits<size_t>::max());

    INFO("Combined slot=" << combined << ", Common slot=" << common << ", Apache err slot=" << apacheError
                          << ", Generic slot=" << generic);
    CHECK(combined < common);
    CHECK(common <= apacheError);
    CHECK(apacheError < generic);

    // Built-in priorities must be set (default-constructed user
    // templates default to 100; a built-in with priority 100 would
    // mean someone forgot to curate). Built-ins should all be
    // strictly below the user-template default bucket.
    for (const RegexTemplate &t : builtins)
    {
        INFO("template: " << t.name);
        CHECK(t.priority < 100);
    }
}

TEST_CASE("autoDetect=false templates are excluded from the probe [regex_templates]", "[regex_templates]")
{
    // Register a high-priority (priority=1, would beat every
    // built-in) user template with `autoDetect=false` and a
    // catch-all pattern; the probe must still pick the matching
    // built-in instead of the higher-priority extra. Reset the
    // extras on the way out so other test cases don't inherit
    // the leftover registration.
    const RegexTemplate hiddenCatchAll{
        .name = "Hidden catch-all",
        .pattern = R"(^(?<line>.*)$)",
        .sampleLines = {"any line"},
        .autoDetect = false,
        .priority = 1,
        .description = "",
    };
    const RegexTemplate extras[] = {hiddenCatchAll};
    SetExtraRegexTemplates(extras);

    const TestLogFile file{"regex_templates_autodetect_off.log"};
    file.Write("Apr 28 04:02:03 host-a systemd: System starting\n"
               "Jun 27 01:47:20 host-b configd[17]: network changed\n");
    const RegexTemplate *detected = DetectRegexTemplate(file.GetFilePath());
    REQUIRE(detected != nullptr);
    CHECK(detected->name == "Syslog (RFC3164)");

    SetExtraRegexTemplates({});
}

TEST_CASE("User-registered templates with autoDetect=true participate in the probe [regex_templates]", "[regex_templates]")
{
    // Symmetric of the autoDetect=false case: an extra with
    // autoDetect=true and a high priority beats a built-in that
    // would otherwise win for the same fixture. The pattern is
    // contrived so neither it nor any built-in could mis-match the
    // syslog samples used elsewhere.
    const RegexTemplate userTemplate{
        .name = "User priority test",
        .pattern = R"(^FOO\s+(?<id>\d+)\s+(?<msg>.*)$)",
        .sampleLines = {"FOO 1 hello"},
        .autoDetect = true,
        .priority = 5,
        .description = "",
    };
    const RegexTemplate extras[] = {userTemplate};
    SetExtraRegexTemplates(extras);

    const TestLogFile file{"regex_templates_user_priority.log"};
    file.Write("FOO 1 hello\n"
               "FOO 2 world\n");
    const RegexTemplate *detected = DetectRegexTemplate(file.GetFilePath());
    REQUIRE(detected != nullptr);
    CHECK(detected->name == "User priority test");

    SetExtraRegexTemplates({});
}

TEST_CASE("FindTemplateByPattern round-trips built-in and user templates [regex_templates]", "[regex_templates]")
{
    // Persistence layer dependency: saved sessions store the raw
    // pattern, and `FindTemplateByPattern` is what the UI calls to
    // re-derive the display name on load. Sweep the built-in
    // catalog AND a stub user slice so a regression that misses
    // one of the two storage tiers fails here loudly.
    for (const RegexTemplate &t : BuiltinRegexTemplates())
    {
        INFO("built-in template: " << t.name);
        const RegexTemplate *found = FindTemplateByPattern(t.pattern);
        REQUIRE(found != nullptr);
        CHECK(found->name == t.name);
    }

    const RegexTemplate userTemplateA{
        .name = "FindByPattern-A",
        .pattern = R"(^ALPHA\s+(?<v>\d+)$)",
        .sampleLines = {"ALPHA 1"},
        .autoDetect = false,
        .priority = 100,
        .description = "",
    };
    const RegexTemplate userTemplateB{
        .name = "FindByPattern-B",
        .pattern = R"(^BETA\s+(?<v>\w+)$)",
        .sampleLines = {"BETA hello"},
        .autoDetect = true,
        .priority = 100,
        .description = "",
    };
    const RegexTemplate extras[] = {userTemplateA, userTemplateB};
    SetExtraRegexTemplates(extras);

    const RegexTemplate *foundA = FindTemplateByPattern(userTemplateA.pattern);
    REQUIRE(foundA != nullptr);
    CHECK(foundA->name == userTemplateA.name);
    const RegexTemplate *foundB = FindTemplateByPattern(userTemplateB.pattern);
    REQUIRE(foundB != nullptr);
    CHECK(foundB->name == userTemplateB.name);

    CHECK(FindTemplateByPattern("definitely not a built-in or user pattern") == nullptr);

    SetExtraRegexTemplates({});
}

TEST_CASE("Every shipped JSON declares a description [regex_templates]", "[regex_templates]")
{
    // Cheap structural assertion: a built-in without a
    // description string would slip past the source-of-truth
    // requirement that every shipped template cite where its
    // pattern came from (lnav, logstash-patterns-core, vendor
    // docs, etc.). The exact text is free-form; we only assert
    // that something is there.
    for (const RegexTemplate &t : BuiltinRegexTemplates())
    {
        INFO("template: " << t.name);
        CHECK_FALSE(t.description.empty());
    }
}
