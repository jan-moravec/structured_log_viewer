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
    t.pattern = R"(^(?<level>\w+)\s+(?<message>.*)$)";
    t.sampleLines = {"info hello", "warn world"};
    t.autoDetect = true;
    t.priority = 25;
    t.description = "Hand-written for the round-trip test.";
    t.continuationMode = ContinuationMode::Indented;
    t.headerAnchor = R"(^\w+)";
    return t;
}

} // namespace

TEST_CASE("RegexTemplate JSON round-trip preserves every field", "[regex_templates][io]")
{
    const RegexTemplate original = MakeExampleTemplate();

    const std::string json = SerializeRegexTemplate(original);
    const RegexTemplate reloaded = ParseRegexTemplate(json);

    CHECK(reloaded.name == original.name);
    CHECK(reloaded.pattern == original.pattern);
    REQUIRE(reloaded.sampleLines.size() == original.sampleLines.size());
    CHECK(reloaded.sampleLines[0] == original.sampleLines[0]);
    CHECK(reloaded.sampleLines[1] == original.sampleLines[1]);
    CHECK(reloaded.autoDetect == original.autoDetect);
    CHECK(reloaded.priority == original.priority);
    CHECK(reloaded.description == original.description);
    CHECK(reloaded.continuationMode == original.continuationMode);
    CHECK(reloaded.headerAnchor == original.headerAnchor);
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
    CHECK(parsed.pattern == "^(?<msg>.*)$");
    REQUIRE(parsed.sampleLines.size() == 1);
    CHECK(parsed.sampleLines[0] == "one line");
    CHECK(parsed.autoDetect == true);
    CHECK(parsed.priority == USER_TEMPLATE_DEFAULT_PRIORITY);
    CHECK(parsed.description.empty());
    CHECK(parsed.continuationMode == ContinuationMode::None);
    CHECK(parsed.headerAnchor.empty());
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

TEST_CASE("SetExtraRegexTemplates exposes user entries to FindTemplateByPattern", "[regex_templates][io]")
{
    // `FindTemplateByPattern` searches the merged registry (built-
    // ins + extras); registering an extra must make a previously-
    // unknown pattern resolve to it. Teardown clears extras so
    // other cases don't see leftover state.
    const RegexTemplate extra{
        .name = "User-defined demo",
        .pattern = R"(^USER\s+(?<id>\d+)$)",
        .sampleLines = {"USER 1"},
        .autoDetect = false,
        .priority = 50,
        .description = "",
    };

    REQUIRE_FALSE(FindTemplateByPattern(extra.pattern).has_value());

    const RegexTemplate extras[] = {extra};
    SetExtraRegexTemplates(extras);

    const auto found = FindTemplateByPattern(extra.pattern);
    REQUIRE(found.has_value());
    CHECK(found->name == extra.name);
    CHECK(found->autoDetect == false);
    CHECK(found->priority == 50);

    SetExtraRegexTemplates({});
    CHECK_FALSE(FindTemplateByPattern(extra.pattern).has_value());
}
