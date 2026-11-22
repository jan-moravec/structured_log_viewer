#include <loglib/core/error.hpp>
#include <loglib/parse/regex_templates.hpp>

#include <catch2/catch_all.hpp>

#include <stdexcept>
#include <string>

using namespace loglib;

namespace
{

RegexTemplate MakeExampleTemplate()
{
    RegexTemplate t;
    t.name = "Example template";
    t.regex.pattern = R"(^(?<level>\w+)\s+(?<message>.*)$)";
    t.sampleLines = {"info hello", "warn world"};
    t.autoDetect = true;
    t.priority = 25;
    t.description = "Hand-written for the round-trip test.";
    t.regex.continuation = ContinuationMode::Indented;
    t.regex.headerAnchor = R"(^\w+)";
    return t;
}

} // namespace

TEST_CASE("RegexTemplate JSON round-trip preserves every field", "[regex_templates][io]")
{
    const RegexTemplate original = MakeExampleTemplate();

    const std::string json = SerializeRegexTemplate(original);
    const RegexTemplate reloaded = ParseRegexTemplate(json);

    CHECK(reloaded.name == original.name);
    CHECK(reloaded.regex.pattern == original.regex.pattern);
    REQUIRE(reloaded.sampleLines.size() == original.sampleLines.size());
    CHECK(reloaded.sampleLines[0] == original.sampleLines[0]);
    CHECK(reloaded.sampleLines[1] == original.sampleLines[1]);
    CHECK(reloaded.autoDetect == original.autoDetect);
    CHECK(reloaded.priority == original.priority);
    CHECK(reloaded.description == original.description);
    CHECK(reloaded.regex.continuation == original.regex.continuation);
    CHECK(reloaded.regex.headerAnchor == original.regex.headerAnchor);
}

TEST_CASE("ContinuationMode name / parse helpers round-trip", "[regex_templates][io]")
{
    CHECK(ContinuationModeName(ContinuationMode::None) == "none");
    CHECK(ContinuationModeName(ContinuationMode::Indented) == "indented");
    CHECK(ContinuationModeName(ContinuationMode::UntilNextHeader) == "untilNextHeader");

    CHECK(ParseContinuationMode("none") == ContinuationMode::None);
    CHECK(ParseContinuationMode("indented") == ContinuationMode::Indented);
    CHECK(ParseContinuationMode("untilNextHeader") == ContinuationMode::UntilNextHeader);
    CHECK_FALSE(ParseContinuationMode("bogus").has_value());
    CHECK_FALSE(ParseContinuationMode("").has_value());
}

TEST_CASE("RegexTemplate JSON parse fills defaults for absent fields", "[regex_templates][io]")
{
    // Optional fields take the defaults documented on the members: autoDetect=true,
    // priority=USER_TEMPLATE_DEFAULT_PRIORITY and an empty description.
    constexpr std::string_view K_MINIMAL_JSON = R"({
        "name": "Minimal template",
        "pattern": "^(?<msg>.*)$",
        "sampleLines": ["one line"]
    })";

    const RegexTemplate parsed = ParseRegexTemplate(K_MINIMAL_JSON);
    CHECK(parsed.name == "Minimal template");
    CHECK(parsed.regex.pattern == "^(?<msg>.*)$");
    REQUIRE(parsed.sampleLines.size() == 1);
    CHECK(parsed.sampleLines[0] == "one line");
    CHECK(parsed.autoDetect == true);
    CHECK(parsed.priority == USER_TEMPLATE_DEFAULT_PRIORITY);
    CHECK(parsed.description.empty());
    CHECK(parsed.regex.continuation == ContinuationMode::None);
    CHECK(parsed.regex.headerAnchor.empty());
}

TEST_CASE("RegexTemplate JSON parse rejects an unknown key and names it", "[regex_templates][io]")
{
    constexpr std::string_view K_UNKNOWN_KEY_JSON = R"({
        "name": "Unknown key template",
        "pattern": "^(?<msg>.*)$",
        "sampleLines": ["x"],
        "futureField": {"someNested": [1, 2, 3]}
    })";

    try
    {
        (void)ParseRegexTemplate(K_UNKNOWN_KEY_JSON);
        FAIL("ParseRegexTemplate accepted an unknown key");
    }
    catch (const FormatError &error)
    {
        CHECK_THAT(std::string(error.what()), Catch::Matchers::ContainsSubstring("futureField"));
    }
}

TEST_CASE("RegexTemplate JSON parse throws FormatError on malformed input", "[regex_templates][io]")
{
    constexpr std::string_view K_BROKEN_JSON = R"({"name": "broken")";
    CHECK_THROWS_AS(ParseRegexTemplate(K_BROKEN_JSON), FormatError);
}

TEST_CASE("WithExtras exposes user entries to FindByPattern", "[regex_templates][io]")
{
    // FindByPattern searches the built-ins and the extras of a catalog. The catalog the extras were added to finds the
    // pattern, and the catalog it was made from does not.
    const RegexTemplate extra{
        .name = "User-defined demo",
        .regex = {.pattern = R"(^USER\s+(?<id>\d+)$)"},
        .sampleLines = {"USER 1"},
        .autoDetect = false,
        .priority = 50,
        .description = "",
    };

    REQUIRE(RegexTemplateCatalog::Builtin().FindByPattern(extra.regex.pattern) == nullptr);

    const RegexTemplateCatalog withExtra = RegexTemplateCatalog::Builtin().WithExtras({extra});

    const RegexTemplate *found = withExtra.FindByPattern(extra.regex.pattern);
    REQUIRE(found != nullptr);
    CHECK(found->name == extra.name);
    CHECK(found->autoDetect == false);
    CHECK(found->priority == 50);

    CHECK(RegexTemplateCatalog::Builtin().FindByPattern(extra.regex.pattern) == nullptr);
}

TEST_CASE("A regex template file keeps its flat keys", "[regex_templates][io]")
{
    RegexTemplate tmpl;
    tmpl.name = "Flat";
    tmpl.regex = {.pattern = R"(^(?<a>.*)$)", .continuation = ContinuationMode::UntilNextHeader, .headerAnchor = "^x"};

    const std::string json = SerializeRegexTemplate(tmpl);
    CHECK(json.contains("\"pattern\""));
    CHECK(json.contains("\"continuationMode\""));
    CHECK(json.contains("\"headerAnchor\""));
    CHECK_FALSE(json.contains("\"regex\""));

    const RegexTemplate parsed = ParseRegexTemplate(json);
    CHECK(parsed.regex == tmpl.regex);
}
