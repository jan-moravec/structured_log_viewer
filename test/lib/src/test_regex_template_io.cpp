#include <loglib/regex_templates.hpp>

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
}

TEST_CASE("RegexTemplate JSON parse fills defaults for absent fields", "[regex_templates][io]")
{
    // Backward compatibility: a JSON file written for the pre-schema
    // template shape should still parse, with the new fields
    // defaulted to the documented values (autoDetect=true,
    // priority=100, description="").
    constexpr std::string_view K_LEGACY_JSON = R"({
        "name": "Legacy template",
        "pattern": "^(?<msg>.*)$",
        "sampleLines": ["one line"]
    })";

    const RegexTemplate parsed = ParseRegexTemplate(K_LEGACY_JSON);
    CHECK(parsed.name == "Legacy template");
    CHECK(parsed.pattern == "^(?<msg>.*)$");
    REQUIRE(parsed.sampleLines.size() == 1);
    CHECK(parsed.sampleLines[0] == "one line");
    CHECK(parsed.autoDetect == true);
    CHECK(parsed.priority == 100);
    CHECK(parsed.description.empty());
}

TEST_CASE("RegexTemplate JSON parse tolerates unknown keys", "[regex_templates][io]")
{
    // `error_on_unknown_keys=false` is shared with `LogConfiguration`
    // / `Theme`. Future schema additions in newer builds shouldn't
    // prevent older builds from loading. The legacy `attribution`
    // key (renamed to `description` mid-cycle) is the closest
    // real-world example: pre-rename user JSONs in the wild must
    // still load -- they just lose that one field's text.
    constexpr std::string_view K_FORWARD_JSON = R"({
        "name": "Forward template",
        "pattern": "^(?<msg>.*)$",
        "sampleLines": ["x"],
        "futureField": {"someNested": [1, 2, 3]},
        "attribution": "legacy citation that should be ignored, not crash"
    })";

    const RegexTemplate parsed = ParseRegexTemplate(K_FORWARD_JSON);
    CHECK(parsed.name == "Forward template");
    // Legacy `attribution` is silently dropped; `description`
    // defaults to empty.
    CHECK(parsed.description.empty());
}

TEST_CASE("RegexTemplate JSON parse throws on malformed input", "[regex_templates][io]")
{
    // Trailing brace missing -> glaze surfaces a parse error, which
    // we wrap in a runtime_error so callers don't need a glaze
    // dependency to catch it.
    constexpr std::string_view K_BROKEN_JSON = R"({"name": "broken")";
    CHECK_THROWS_AS(ParseRegexTemplate(K_BROKEN_JSON), std::runtime_error);
}

TEST_CASE("SetExtraRegexTemplates exposes user entries to FindTemplateByPattern", "[regex_templates][io]")
{
    // The merged registry (built-ins + extras) is what
    // `FindTemplateByPattern` searches; registering an extra should
    // make a previously-unknown pattern resolve to the registered
    // template. The teardown step clears extras so other test cases
    // don't see leftover state.
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
