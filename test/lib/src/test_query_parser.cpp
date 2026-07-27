#include <loglib/filter_expression.hpp>
#include <loglib/query_parser.hpp>

#include <catch2/catch_all.hpp>

#include <cstdint>
#include <string>
#include <variant>

using namespace loglib;

namespace
{

/// Convenience: extract the Leaf out of a parsed expression, or
/// `nullptr` when the top-level node isn't a Leaf. Keeps the test
/// body focused on assertions rather than variant plumbing.
const LeafRule *AsLeaf(const FilterExpression &expr)
{
    const auto *leaf = std::get_if<FilterExpression::Leaf>(&expr.node);
    return (leaf != nullptr) ? &leaf->rule : nullptr;
}

/// Convenience: parse or `FAIL_CHECK` and return a default value.
FilterExpression ParseOrFail(std::string_view input)
{
    auto parsed = ParseQuery(input);
    if (!parsed.has_value())
    {
        FAIL_CHECK("parse failed at offset " << parsed.error().offset << ": " << parsed.error().message);
        return {};
    }
    return std::move(*parsed);
}

} // namespace

TEST_CASE("ParseQuery: empty input is match-all", "[query_parser]")
{
    const auto parsed = ParseQuery("");
    REQUIRE(parsed.has_value());
    CHECK(IsMatchAll(*parsed));
    const auto whitespaceOnly = ParseQuery("   \t\n");
    REQUIRE(whitespaceOnly.has_value());
    CHECK(IsMatchAll(*whitespaceOnly));
}

TEST_CASE("ParseQuery: string Contains leaf", "[query_parser]")
{
    const auto expr = ParseOrFail("service:auth");
    const LeafRule *leaf = AsLeaf(expr);
    REQUIRE(leaf != nullptr);
    CHECK(leaf->type == LeafRule::Type::String);
    CHECK(leaf->matchType == LeafRule::Match::Contains);
    REQUIRE(leaf->columnKeys.size() == 1);
    CHECK(leaf->columnKeys.front() == "service");
    REQUIRE(leaf->filterString.has_value());
    CHECK(*leaf->filterString == "auth");
}

TEST_CASE("ParseQuery: string Exactly is always quoted-only", "[query_parser]")
{
    const auto expr = ParseOrFail("service=\"auth\"");
    const LeafRule *leaf = AsLeaf(expr);
    REQUIRE(leaf != nullptr);
    CHECK(leaf->type == LeafRule::Type::String);
    CHECK(leaf->matchType == LeafRule::Match::Exactly);
    CHECK(*leaf->filterString == "auth");
}

TEST_CASE("ParseQuery: string Regex uses /.../ delimiters", "[query_parser]")
{
    const auto expr = ParseOrFail("msg ~ /err(or)?/");
    const LeafRule *leaf = AsLeaf(expr);
    REQUIRE(leaf != nullptr);
    CHECK(leaf->type == LeafRule::Type::String);
    CHECK(leaf->matchType == LeafRule::Match::RegularExpression);
    CHECK(*leaf->filterString == "err(or)?");
}

TEST_CASE("ParseQuery: string Wildcard uses % operator", "[query_parser]")
{
    const auto expr = ParseOrFail("path%\"*.log\"");
    const LeafRule *leaf = AsLeaf(expr);
    REQUIRE(leaf != nullptr);
    CHECK(leaf->matchType == LeafRule::Match::Wildcard);
    CHECK(*leaf->filterString == "*.log");
}

TEST_CASE("ParseQuery: numeric equality", "[query_parser]")
{
    const auto expr = ParseOrFail("latency=42");
    const LeafRule *leaf = AsLeaf(expr);
    REQUIRE(leaf != nullptr);
    CHECK(leaf->type == LeafRule::Type::Number);
    REQUIRE(leaf->filterMinValue.has_value());
    REQUIRE(leaf->filterMaxValue.has_value());
    CHECK(*leaf->filterMinValue == 42.0);
    CHECK(*leaf->filterMaxValue == 42.0);
}

TEST_CASE("ParseQuery: numeric one-sided comparisons", "[query_parser]")
{
    SECTION(">= is inclusive")
    {
        const auto expr = ParseOrFail("latency >= 100");
        const LeafRule *leaf = AsLeaf(expr);
        REQUIRE(leaf != nullptr);
        REQUIRE(leaf->filterMinValue.has_value());
        CHECK(*leaf->filterMinValue == 100.0);
        CHECK_FALSE(leaf->filterMaxValue.has_value());
    }
    SECTION("> is strict (ULP-adjacent)")
    {
        const auto expr = ParseOrFail("latency > 100");
        const LeafRule *leaf = AsLeaf(expr);
        REQUIRE(leaf != nullptr);
        REQUIRE(leaf->filterMinValue.has_value());
        CHECK(*leaf->filterMinValue > 100.0);
    }
    SECTION("<= is inclusive")
    {
        const auto expr = ParseOrFail("latency <= 100");
        const LeafRule *leaf = AsLeaf(expr);
        REQUIRE(leaf != nullptr);
        REQUIRE(leaf->filterMaxValue.has_value());
        CHECK(*leaf->filterMaxValue == 100.0);
    }
}

TEST_CASE("ParseQuery: boolean equality", "[query_parser]")
{
    const auto expr = ParseOrFail("succeeded=true");
    const LeafRule *leaf = AsLeaf(expr);
    REQUIRE(leaf != nullptr);
    CHECK(leaf->type == LeafRule::Type::Boolean);
    REQUIRE(leaf->filterValues.size() == 1);
    CHECK(leaf->filterValues.front() == "true");
}

TEST_CASE("ParseQuery: enum list", "[query_parser]")
{
    const auto expr = ParseOrFail("level in [Info, Warn, Error]");
    const LeafRule *leaf = AsLeaf(expr);
    REQUIRE(leaf != nullptr);
    CHECK(leaf->type == LeafRule::Type::Enumeration);
    REQUIRE(leaf->filterValues.size() == 3);
    CHECK(leaf->filterValues[0] == "Info");
    CHECK(leaf->filterValues[1] == "Warn");
    CHECK(leaf->filterValues[2] == "Error");
}

TEST_CASE("ParseQuery: numeric range in [min..max]", "[query_parser]")
{
    const auto expr = ParseOrFail("latency in [10..100]");
    const LeafRule *leaf = AsLeaf(expr);
    REQUIRE(leaf != nullptr);
    CHECK(leaf->type == LeafRule::Type::Number);
    REQUIRE(leaf->filterMinValue.has_value());
    REQUIRE(leaf->filterMaxValue.has_value());
    CHECK(*leaf->filterMinValue == 10.0);
    CHECK(*leaf->filterMaxValue == 100.0);
}

TEST_CASE("ParseQuery: numeric range with open sides", "[query_parser]")
{
    SECTION("open lower bound")
    {
        const auto expr = ParseOrFail("latency in [..100]");
        const LeafRule *leaf = AsLeaf(expr);
        REQUIRE(leaf != nullptr);
        CHECK_FALSE(leaf->filterMinValue.has_value());
        REQUIRE(leaf->filterMaxValue.has_value());
        CHECK(*leaf->filterMaxValue == 100.0);
    }
    SECTION("open upper bound")
    {
        const auto expr = ParseOrFail("latency in [10..]");
        const LeafRule *leaf = AsLeaf(expr);
        REQUIRE(leaf != nullptr);
        REQUIRE(leaf->filterMinValue.has_value());
        CHECK(*leaf->filterMinValue == 10.0);
        CHECK_FALSE(leaf->filterMaxValue.has_value());
    }
}

TEST_CASE("ParseQuery: time range recognises ISO literals", "[query_parser]")
{
    const auto expr = ParseOrFail("ts in [2024-01-02T03:04:05Z..2024-01-03T00:00:00Z]");
    const LeafRule *leaf = AsLeaf(expr);
    REQUIRE(leaf != nullptr);
    CHECK(leaf->type == LeafRule::Type::Time);
    REQUIRE(leaf->filterBegin.has_value());
    REQUIRE(leaf->filterEnd.has_value());
    // Sanity: begin < end and both are non-negative.
    CHECK(*leaf->filterBegin > 0);
    CHECK(*leaf->filterEnd > *leaf->filterBegin);
}

TEST_CASE("ParseQuery: time comparison recognises ISO literal", "[query_parser]")
{
    const auto expr = ParseOrFail("ts >= 2024-01-02T00:00:00Z");
    const LeafRule *leaf = AsLeaf(expr);
    REQUIRE(leaf != nullptr);
    CHECK(leaf->type == LeafRule::Type::Time);
    REQUIRE(leaf->filterBegin.has_value());
    CHECK(*leaf->filterBegin > 0);
}

TEST_CASE("ParseQuery: AND / OR / NOT precedence", "[query_parser]")
{
    // `a AND b OR c` should parse as `(a AND b) OR c`.
    const auto expr = ParseOrFail("service:auth AND level:error OR path:api");
    const auto *orNode = std::get_if<FilterExpression::Or>(&expr.node);
    REQUIRE(orNode != nullptr);
    REQUIRE(orNode->children.size() == 2);
    const auto *inner = std::get_if<FilterExpression::And>(&orNode->children.front().node);
    REQUIRE(inner != nullptr);
    CHECK(inner->children.size() == 2);
}

TEST_CASE("ParseQuery: implicit AND", "[query_parser]")
{
    const auto expr = ParseOrFail("service:auth level:error");
    const auto *andNode = std::get_if<FilterExpression::And>(&expr.node);
    REQUIRE(andNode != nullptr);
    REQUIRE(andNode->children.size() == 2);
}

TEST_CASE("ParseQuery: NOT wraps a leaf", "[query_parser]")
{
    const auto expr = ParseOrFail("NOT service:auth");
    const auto *notNode = std::get_if<FilterExpression::Not>(&expr.node);
    REQUIRE(notNode != nullptr);
    REQUIRE(notNode->child != nullptr);
    CHECK(AsLeaf(*notNode->child) != nullptr);
}

TEST_CASE("ParseQuery: parenthesised group flips precedence", "[query_parser]")
{
    // Without parens this would be `(a) AND (b OR c)` -> the group
    // must be an OR node under the AND.
    const auto expr = ParseOrFail("service:auth AND (level:error OR level:warn)");
    const auto *andNode = std::get_if<FilterExpression::And>(&expr.node);
    REQUIRE(andNode != nullptr);
    REQUIRE(andNode->children.size() == 2);
    const auto *orNode = std::get_if<FilterExpression::Or>(&andNode->children[1].node);
    REQUIRE(orNode != nullptr);
    CHECK(orNode->children.size() == 2);
}

TEST_CASE("ParseQuery: symbolic operators &&, ||, !", "[query_parser]")
{
    const auto expr = ParseOrFail("service:auth && !(level:info) || path:api");
    const auto *orNode = std::get_if<FilterExpression::Or>(&expr.node);
    REQUIRE(orNode != nullptr);
    REQUIRE(orNode->children.size() == 2);
}

TEST_CASE("ParseQuery: quoted column preserves whitespace", "[query_parser]")
{
    const auto expr = ParseOrFail("\"span id\":\"abc def\"");
    const LeafRule *leaf = AsLeaf(expr);
    REQUIRE(leaf != nullptr);
    REQUIRE(leaf->columnKeys.size() == 1);
    CHECK(leaf->columnKeys.front() == "span id");
    CHECK(*leaf->filterString == "abc def");
}

TEST_CASE("ParseQuery: errors carry a caret position", "[query_parser]")
{
    SECTION("missing operator")
    {
        const auto parsed = ParseQuery("service");
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error().offset > 0);
    }
    SECTION("unterminated string")
    {
        const auto parsed = ParseQuery("service:\"unterm");
        REQUIRE_FALSE(parsed.has_value());
    }
    SECTION("unterminated regex")
    {
        const auto parsed = ParseQuery("service ~ /unterm");
        REQUIRE_FALSE(parsed.has_value());
    }
    SECTION("trailing tokens")
    {
        const auto parsed = ParseQuery("service:auth junk");
        // `service:auth junk` is a valid implicit-AND if `junk`
        // becomes a leaf on its own -- but `junk` has no operator,
        // so the parser errors on that.
        REQUIRE_FALSE(parsed.has_value());
    }
}

TEST_CASE("FormatExpression: round-trips simple leaves", "[query_parser][pretty]")
{
    struct Case
    {
        std::string query;
    };
    const Case cases[] = {
        {"service:auth"},
        {"service=\"exact match\""},
        {"latency=42"},
        {"latency>=100"},
        {"latency<=100"},
        {"succeeded=true"},
        {"succeeded=false"},
        {"level in [Info, Warn]"},
        {"NOT service:auth"},
    };
    for (const auto &c : cases)
    {
        CAPTURE(c.query);
        auto parsed = ParseQuery(c.query);
        REQUIRE(parsed.has_value());
        const std::string formatted = FormatExpression(*parsed);
        auto reparsed = ParseQuery(formatted);
        REQUIRE(reparsed.has_value());
        // The AST must equal the original AST, not the string --
        // the pretty printer is free to normalise whitespace and
        // choose canonical spellings.
        CHECK(*parsed == *reparsed);
    }
}

TEST_CASE("FormatExpression: round-trips composite trees", "[query_parser][pretty]")
{
    const std::string query = "service:auth AND (level in [Warn, Error] OR NOT path:health)";
    const auto parsed = ParseQuery(query);
    REQUIRE(parsed.has_value());
    const std::string formatted = FormatExpression(*parsed);
    const auto reparsed = ParseQuery(formatted);
    REQUIRE(reparsed.has_value());
    CHECK(*parsed == *reparsed);
}

TEST_CASE("FormatExpression: numeric range renders as in [..]", "[query_parser][pretty]")
{
    const auto parsed = ParseOrFail("latency in [10..100]");
    const std::string out = FormatExpression(parsed);
    CHECK(out.find("in [") != std::string::npos);
    CHECK(out.find("..") != std::string::npos);
}
