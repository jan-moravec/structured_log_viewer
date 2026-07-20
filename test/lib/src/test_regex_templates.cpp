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
        slug, [](char c) { return !static_cast<bool>(std::isalnum(static_cast<unsigned char>(c))); }, '_'
    );
    return slug;
}

/// Drive @p pattern through `RegexParser` over a synthetic file
/// of @p lines. Wraps the static pipeline plus the
/// `LogFactory`/`ParseFile`-style sink so we can assert on row /
/// error counts directly. Returns the `ParseResult`.
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
    // For every built-in entry: compile the pattern and assert
    // that every sample line emits exactly one row (no errors).
    // Doubles as a syntax check (PCRE2 wouldn't compile a
    // malformed pattern) and a behavioural check (named groups
    // wired as the template documents).
    const auto builtins = BuiltinRegexTemplates();
    REQUIRE_FALSE(builtins.empty());

    for (const RegexTemplate &t : builtins)
    {
        INFO("template: " << t.name);
        REQUIRE_FALSE(t.sampleLines.empty());

        // ParseFile needs at least two non-blank lines or the
        // regex auto-detect probe declines. Drive the
        // pinned-pattern RegexParser directly so single-sample
        // templates still parse.
        const ParseResult result = ParseLinesWith(
            t.pattern, std::span<const std::string>(t.sampleLines), "regex_templates_" + SlugifyName(t.name) + ".log"
        );
        CHECK(result.errors.empty());
        CHECK(result.data.Lines().size() == t.sampleLines.size());
    }
}

TEST_CASE("DetectRegexTemplate identifies syslog samples [regex_templates]", "[regex_templates]")
{
    // Cross-template check: syslog samples aren't misidentified
    // as one of the other formats. The probe scans templates in
    // registry order and the first hit wins.
    const TestLogFile file{"regex_templates_detect.log"};
    file.Write(
        "Apr 28 04:02:03 host-a systemd: System starting\n"
        "Jun 27 01:47:20 host-b configd[17]: network changed\n"
    );
    const auto detected = DetectRegexTemplate(file.GetFilePath());
    REQUIRE(detected.has_value());
    CHECK(detected->name == "Syslog (RFC3164)");
}

TEST_CASE("DetectRegexTemplate identifies Apache CLF samples [regex_templates]", "[regex_templates]")
{
    const TestLogFile file{"regex_templates_detect_clf.log"};
    file.Write(
        R"(127.0.0.1 - frank [10/Oct/2000:13:55:36 -0700] "GET /apache_pb.gif HTTP/1.0" 200 2326)"
        "\n"
        R"(10.1.10.51 - - [23/Dec/2014:21:20:35 +0000] "POST /api/1/rest/foo HTTP/1.1" 200 -)"
        "\n"
    );
    const auto detected = DetectRegexTemplate(file.GetFilePath());
    REQUIRE(detected.has_value());
    CHECK(detected->name == "Apache/nginx Common Log Format");
}

TEST_CASE("FindBuiltinByPattern round-trips every registry entry [regex_templates]", "[regex_templates]")
{
    // GUI / config persistence uses this lookup to recover the
    // display name from a saved pattern. Regression guard against
    // a future refactor that changes string identity.
    for (const RegexTemplate &t : BuiltinRegexTemplates())
    {
        INFO("template: " << t.name);
        const auto found = FindBuiltinByPattern(t.pattern);
        REQUIRE(found.has_value());
        CHECK(found->name == t.name);
    }

    // Unknown pattern (user-supplied custom) returns nullopt.
    CHECK_FALSE(FindBuiltinByPattern("definitely not a built-in pattern").has_value());
}

TEST_CASE(
    "Built-in regex templates are returned in priority-then-document order [regex_templates]", "[regex_templates]"
)
{
    // The probe loop scans templates in `priority` ascending, so
    // the curated order is load-bearing. Assert the relative
    // order of the Apache-family templates plus the generic
    // fallback to lock the documented "Combined before Common
    // before Apache error before Generic" invariant.
    //
    // Purely structural — behavioural coverage of "the right
    // template wins for an ambiguous file" lives in the existing
    // detect cases.
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

    INFO(
        "Combined slot=" << combined << ", Common slot=" << common << ", Apache err slot=" << apacheError
                         << ", Generic slot=" << generic
    );
    CHECK(combined < common);
    CHECK(common <= apacheError);
    CHECK(apacheError < generic);

    // Built-in priorities must be set — user templates default
    // to `USER_TEMPLATE_DEFAULT_PRIORITY`, so a built-in at that
    // value would mean someone forgot to curate. All built-ins
    // should be strictly below the user-template default bucket.
    for (const RegexTemplate &t : builtins)
    {
        INFO("template: " << t.name);
        CHECK(t.priority < USER_TEMPLATE_DEFAULT_PRIORITY);
    }
}

TEST_CASE("autoDetect=false templates are excluded from the probe [regex_templates]", "[regex_templates]")
{
    // Register a high-priority (priority=1, would beat every
    // built-in) user template with `autoDetect=false` and a
    // catch-all pattern. The probe must still pick the matching
    // built-in rather than the higher-priority extra. Reset
    // extras on the way out so other test cases don't inherit
    // the registration.
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
    file.Write(
        "Apr 28 04:02:03 host-a systemd: System starting\n"
        "Jun 27 01:47:20 host-b configd[17]: network changed\n"
    );
    const auto detected = DetectRegexTemplate(file.GetFilePath());
    REQUIRE(detected.has_value());
    CHECK(detected->name == "Syslog (RFC3164)");

    SetExtraRegexTemplates({});
}

TEST_CASE(
    "User-registered templates with autoDetect=true participate in the probe [regex_templates]", "[regex_templates]"
)
{
    // Symmetric of the autoDetect=false case: an extra with
    // `autoDetect=true` and a high priority beats a built-in that
    // would otherwise win. The pattern is contrived so it can't
    // mismatch syslog samples used elsewhere.
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
    file.Write(
        "FOO 1 hello\n"
        "FOO 2 world\n"
    );
    const auto detected = DetectRegexTemplate(file.GetFilePath());
    REQUIRE(detected.has_value());
    CHECK(detected->name == "User priority test");

    SetExtraRegexTemplates({});
}

TEST_CASE("FindTemplateByPattern round-trips built-in and user templates [regex_templates]", "[regex_templates]")
{
    // Persistence layer dependency: saved sessions store the raw
    // pattern; `FindTemplateByPattern` is what the UI calls to
    // re-derive the display name on load. Sweep the built-in
    // catalog AND a stub user slice so a regression missing
    // either tier fails here loudly.
    for (const RegexTemplate &t : BuiltinRegexTemplates())
    {
        INFO("built-in template: " << t.name);
        const auto found = FindTemplateByPattern(t.pattern);
        REQUIRE(found.has_value());
        CHECK(found->name == t.name);
    }

    const RegexTemplate userTemplateA{
        .name = "FindByPattern-A",
        .pattern = R"(^ALPHA\s+(?<v>\d+)$)",
        .sampleLines = {"ALPHA 1"},
        .autoDetect = false,
        .priority = USER_TEMPLATE_DEFAULT_PRIORITY,
        .description = "",
    };
    const RegexTemplate userTemplateB{
        .name = "FindByPattern-B",
        .pattern = R"(^BETA\s+(?<v>\w+)$)",
        .sampleLines = {"BETA hello"},
        .autoDetect = true,
        .priority = USER_TEMPLATE_DEFAULT_PRIORITY,
        .description = "",
    };
    const RegexTemplate extras[] = {userTemplateA, userTemplateB};
    SetExtraRegexTemplates(extras);

    const auto foundA = FindTemplateByPattern(userTemplateA.pattern);
    REQUIRE(foundA.has_value());
    CHECK(foundA->name == userTemplateA.name);
    const auto foundB = FindTemplateByPattern(userTemplateB.pattern);
    REQUIRE(foundB.has_value());
    CHECK(foundB->name == userTemplateB.name);

    CHECK_FALSE(FindTemplateByPattern("definitely not a built-in or user pattern").has_value());

    SetExtraRegexTemplates({});
}

TEST_CASE("Every shipped JSON declares a description [regex_templates]", "[regex_templates]")
{
    // Cheap structural assertion: a built-in without a
    // description would slip past the source-of-truth
    // requirement that every shipped template cites where its
    // pattern came from (lnav, logstash-patterns-core, vendor
    // docs, ...). Text is free-form; only check that something
    // is there.
    for (const RegexTemplate &t : BuiltinRegexTemplates())
    {
        INFO("template: " << t.name);
        CHECK_FALSE(t.description.empty());
    }
}

TEST_CASE("Built-ins probe before user templates regardless of priority [regex_templates]", "[regex_templates]")
{
    // Regression guard for `CompiledProbeSnapshot`'s ordering
    // invariant. `Syslog (RFC3164)` ships with priority=10; a
    // user template with priority=1 (which would outrank every
    // built-in on a priority-only sort) must still lose the
    // probe race for a syslog-shaped file, because the compile
    // cache preserves the two-tier "built-ins first" ordering.
    //
    // The user template's `^.*$` matches every non-blank line,
    // so it would definitely win if the probe ordering broke.
    const RegexTemplate userCatchAll{
        .name = "User catch-all with low priority",
        .pattern = R"(^.*$)",
        .sampleLines = {"any line"},
        .autoDetect = true,
        .priority = 1,
        .description = "",
    };
    const RegexTemplate extras[] = {userCatchAll};
    SetExtraRegexTemplates(extras);

    const TestLogFile file{"regex_templates_builtins_first.log"};
    file.Write(
        "Apr 28 04:02:03 host-a systemd: System starting\n"
        "Jun 27 01:47:20 host-b configd[17]: network changed\n"
    );
    const auto detected = DetectRegexTemplate(file.GetFilePath());
    REQUIRE(detected.has_value());
    CHECK(detected->name == "Syslog (RFC3164)");

    SetExtraRegexTemplates({});
}

TEST_CASE("Unanchored user templates cannot substring-match the probe [regex_templates]", "[regex_templates]")
{
    // Regression guard for `MatchesFullyForProbe` passing
    // `PCRE2_ANCHORED | PCRE2_ENDANCHORED`. Without those flags
    // a user template like `USER (?<id>\d+)` (no `^...$`) would
    // partial-match any line mentioning `USER 42`, silently
    // claiming files it wasn't meant for and producing junk
    // afterwards. With the anchor flags in place, a substring-
    // only match must fail auto-detect and let the properly
    // anchored syslog template win.
    const RegexTemplate userUnanchored{
        .name = "User unanchored substring",
        .pattern = R"(USER\s+(?<id>\d+))",
        .sampleLines = {"USER 42"},
        .autoDetect = true,
        .priority = 1,
        .description = "",
    };
    const RegexTemplate extras[] = {userUnanchored};
    SetExtraRegexTemplates(extras);

    // Fixture lines mention "USER 42" mid-line so an unanchored
    // probe would match. The anchored probe must refuse and let
    // syslog claim the file.
    const TestLogFile file{"regex_templates_unanchored_user.log"};
    file.Write(
        "Apr 28 04:02:03 host-a systemd: connect USER 42 ok\n"
        "Jun 27 01:47:20 host-b configd[17]: reload USER 42 done\n"
    );
    const auto detected = DetectRegexTemplate(file.GetFilePath());
    REQUIRE(detected.has_value());
    CHECK(detected->name == "Syslog (RFC3164)");

    SetExtraRegexTemplates({});
}

TEST_CASE(
    "Java template captures logger separately from message for colon-fused lines [regex_templates]", "[regex_templates]"
)
{
    // Regression guard for the `\s*[-:]\s+` separator in the Java
    // template. Logback's default layout uses `logger - message`;
    // log4j and some vendor overrides use the fused-colon shape
    // `logger: message` (no space between the logger name and the
    // colon). The earlier `\s+[-:]?\s+` shape required whitespace
    // on both sides of the separator, so the entire optional
    // logger group failed on colon-fused lines and the message
    // column swallowed the logger name — silent data loss the
    // "Built-in regex templates compile and parse their sample
    // lines" sweep didn't catch because it only asserted a row
    // gets emitted.
    // Look the pattern up out-of-band via the template name
    // rather than hardcoding the pattern string here; if someone
    // renames or reshapes the template we want the test to fail
    // loudly with the assertion below, not silently via a stale
    // pattern literal.
    std::string javaPattern;
    for (const RegexTemplate &t : BuiltinRegexTemplates())
    {
        if (t.name == "Java / log4j / SLF4J Logback")
        {
            javaPattern = t.pattern;
            break;
        }
    }
    REQUIRE_FALSE(javaPattern.empty());
    const RegexParser parser{javaPattern};

    const TestLogFile file{"regex_templates_java_colon_fused.log"};
    file.Write(
        "2024-04-28 04:02:05.789 ERROR [pool-1-thread-3] com.example.Worker$Inner: Task failed after 3 retries\n"
        "2024-04-28 04:02:06.123 INFO  [main] com.example.App - Application starting\n"
    );

    const ParseResult result = ParseFile(parser, file.GetFilePath());
    REQUIRE(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 2);

    // Row 0: colon-fused. `logger` must be the class name, and
    // `message` must be the payload only (no logger prefix folded
    // in).
    const auto row0logger = result.data.Lines()[0].GetValue("logger");
    const auto row0message = result.data.Lines()[0].GetValue("message");
    REQUIRE(std::holds_alternative<std::string_view>(row0logger));
    CHECK(std::get<std::string_view>(row0logger) == std::string_view{"com.example.Worker$Inner"});
    REQUIRE(std::holds_alternative<std::string_view>(row0message));
    CHECK(std::get<std::string_view>(row0message) == std::string_view{"Task failed after 3 retries"});

    // Row 1: dash-with-spaces, the Logback-default shape. Same
    // expectations — regression guard both shapes symmetrically.
    const auto row1logger = result.data.Lines()[1].GetValue("logger");
    const auto row1message = result.data.Lines()[1].GetValue("message");
    REQUIRE(std::holds_alternative<std::string_view>(row1logger));
    CHECK(std::get<std::string_view>(row1logger) == std::string_view{"com.example.App"});
    REQUIRE(std::holds_alternative<std::string_view>(row1message));
    CHECK(std::get<std::string_view>(row1message) == std::string_view{"Application starting"});
}
