#include <loglib/filter_expression.hpp>
#include <loglib/query_parser.hpp>

#include <catch2/catch_all.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
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

// Regression: `col in [true, false]` is the wire form the pretty
// printer emits for `LeafRule::Type::Boolean` (multi-value bool).
// Before the fix `FinishListLeaf` unconditionally stamped the leaf
// as `Enumeration`, so the compiled predicate walked the string-set
// fallback against a `Boolean` column and matched zero rows.
TEST_CASE("ParseQuery: bool-only in-list becomes Type::Boolean", "[query_parser]")
{
    SECTION("both values")
    {
        const auto expr = ParseOrFail("succeeded in [true, false]");
        const LeafRule *leaf = AsLeaf(expr);
        REQUIRE(leaf != nullptr);
        CHECK(leaf->type == LeafRule::Type::Boolean);
        REQUIRE(leaf->filterValues.size() == 2);
        CHECK(leaf->filterValues[0] == "true");
        CHECK(leaf->filterValues[1] == "false");
    }
    SECTION("single true")
    {
        const auto expr = ParseOrFail("succeeded in [true]");
        const LeafRule *leaf = AsLeaf(expr);
        REQUIRE(leaf != nullptr);
        CHECK(leaf->type == LeafRule::Type::Boolean);
        REQUIRE(leaf->filterValues.size() == 1);
        CHECK(leaf->filterValues.front() == "true");
    }
    SECTION("case-preserved input normalises to lowercase")
    {
        // Boolean rules canonicalise to lowercase so the round-trip
        // through `FormatExpression` (which emits lowercase) matches.
        const auto expr = ParseOrFail("succeeded in [True, FALSE]");
        const LeafRule *leaf = AsLeaf(expr);
        REQUIRE(leaf != nullptr);
        CHECK(leaf->type == LeafRule::Type::Boolean);
        REQUIRE(leaf->filterValues.size() == 2);
        CHECK(leaf->filterValues[0] == "true");
        CHECK(leaf->filterValues[1] == "false");
    }
    SECTION("mixed list stays Enumeration")
    {
        // `[true, "yes"]` isn't a canonical Boolean spelling — keep it
        // on the Enumeration path so hand-typed heterogeneous lists
        // aren't silently misclassified.
        const auto expr = ParseOrFail("field in [true, \"yes\"]");
        const LeafRule *leaf = AsLeaf(expr);
        REQUIRE(leaf != nullptr);
        CHECK(leaf->type == LeafRule::Type::Enumeration);
    }
}

// Regression: a Boolean multi-value leaf must round-trip through
// `FormatExpression` -> `ParseQuery` back to the same AST. The wire
// form is `col in [true, false]`; before the parser fix it came
// back as `Enumeration`, so the ASTs no longer compared equal and
// the compiled predicate matched zero rows.
TEST_CASE("FormatExpression: boolean multi-value round-trips through the parser", "[query_parser][pretty]")
{
    FilterExpression source;
    LeafRule rule;
    rule.type = LeafRule::Type::Boolean;
    rule.columnKeys = {"succeeded"};
    rule.filterValues = {"true", "false"};
    source.node = FilterExpression::Leaf{rule};

    const std::string formatted = FormatExpression(source);
    const auto reparsed = ParseQuery(formatted);
    REQUIRE(reparsed.has_value());
    CHECK(source == *reparsed);
}

// Regression: `FormatTimestampMicros` used C++ truncation on the
// second/fraction split, so a negative sub-second value (e.g.
// `-500'000` = 500ms before epoch) rendered on the wrong side of
// the epoch. The correct pretty-print borrows one whole second and
// reflects the fraction, so `-500'000` -> `1969-12-31T23:59:59...`.
TEST_CASE(
    "FormatExpression: sub-second negative timestamp borrows a whole second", "[query_parser][pretty][regression]"
)
{
    // Build a Time leaf directly against a known negative
    // begin-bound so we can eyeball the pretty-print output. The
    // parser round-trips ISO strings back to microseconds, so the
    // AST equality below is the load-bearing invariant.
    FilterExpression source;
    LeafRule rule;
    rule.type = LeafRule::Type::Time;
    rule.columnKeys = {"ts"};
    rule.filterBegin = static_cast<std::int64_t>(-500'000);
    source.node = FilterExpression::Leaf{rule};

    const std::string formatted = FormatExpression(source);
    // Expect the borrowed second (23:59:59.500000Z) in the output;
    // the previous buggy path emitted the epoch itself.
    CHECK(formatted.contains("1969-12-31T23:59:59.500000Z"));
    CHECK_FALSE(formatted.contains("1970-01-01T00:00:00.500000Z"));

    const auto reparsed = ParseQuery(formatted);
    REQUIRE(reparsed.has_value());
    CHECK(source == *reparsed);
}

// Regression: the top-level match-all tree (`And{}`) must render
// as the empty string so it round-trips through `ParseQuery`.
// Before the fix, `FormatExpression(FilterExpression{})` emitted
// `*` -- which is not in the grammar and reparses to a
// `unexpected character '*'` error, silently breaking the
// header's advertised round-trip guarantee.
TEST_CASE("FormatExpression: match-all round-trips as empty string", "[query_parser][pretty][regression]")
{
    const FilterExpression matchAll{};
    REQUIRE(IsMatchAll(matchAll));

    const std::string formatted = FormatExpression(matchAll);
    CHECK(formatted.empty());

    const auto reparsed = ParseQuery(formatted);
    REQUIRE(reparsed.has_value());
    CHECK(IsMatchAll(*reparsed));
}

// Regression: `LexNumber` used to accept a dangling exponent like
// `1e` or `1e+` as a valid Number token, deferring the diagnostic
// to `std::from_chars` at leaf-payload parsing time. The message
// then pointed at the whole token instead of the offending 'e',
// hiding the true cause. The lexer now rejects the token up front
// so the caret lands on the `e`.
TEST_CASE("ParseQuery: numeric literal with dangling exponent errors at the 'e'", "[query_parser][regression]")
{
    SECTION("bare `1e`")
    {
        const auto parsed = ParseQuery("latency=1e");
        REQUIRE_FALSE(parsed.has_value());
        // "latency=1e" -- the `e` sits at offset 9 (0-based).
        CHECK(parsed.error().offset == 9);
        CHECK(parsed.error().message.contains("exponent"));
    }
    SECTION("signed `1e+` and `1e-` both fail")
    {
        for (const std::string_view q : {"latency=1e+", "latency=1e-"})
        {
            CAPTURE(q);
            const auto parsed = ParseQuery(q);
            REQUIRE_FALSE(parsed.has_value());
        }
    }
    SECTION("valid `1e2` still parses")
    {
        const auto parsed = ParseQuery("latency=1e2");
        REQUIRE(parsed.has_value());
    }
}

// Regression: `FormatTimestampMicros` used to delegate to
// `gmtime_s` / `gmtime_r`. MSVC's `gmtime_s` returns `EINVAL` for
// every `time_t` below `-43200`, so *any* bound more than twelve
// hours before the epoch hit the `epoch_micros:<n>` fallback -- and
// that marker is not part of the grammar, so the round-trip through
// the Advanced editor turned into a parse error. The formatter now
// does the civil-date arithmetic itself, exactly inverting
// `ParseIsoTimestamp`.
TEST_CASE("FormatExpression: pre-epoch timestamps round-trip", "[query_parser][pretty][regression]")
{
    struct Case
    {
        std::int64_t micros;
        std::string_view expectedIso;
    };
    // `-43'200'000'001` is one microsecond past the point where
    // `gmtime_s` starts failing; the rest walk further back through a
    // leap day and a century boundary (1900 is *not* a leap year, so
    // a formatter that got the rule wrong lands on the wrong day).
    const std::array<Case, 5> cases{
        Case{.micros = -43'200'000'001, .expectedIso = "1969-12-31T11:59:59.999999Z"},
        Case{.micros = -86'400'000'000, .expectedIso = "1969-12-31T00:00:00.000000Z"},
        Case{.micros = -31'536'000'000'000, .expectedIso = "1969-01-01T00:00:00.000000Z"},
        Case{.micros = -2'208'988'800'000'000, .expectedIso = "1900-01-01T00:00:00.000000Z"},
        Case{.micros = 951'782'400'000'000, .expectedIso = "2000-02-29T00:00:00.000000Z"},
    };
    for (const auto &c : cases)
    {
        CAPTURE(c.micros);
        FilterExpression source;
        LeafRule rule;
        rule.type = LeafRule::Type::Time;
        rule.columnKeys = {"ts"};
        rule.filterBegin = c.micros;
        source.node = FilterExpression::Leaf{rule};

        const std::string formatted = FormatExpression(source);
        CAPTURE(formatted);
        CHECK(formatted.contains(c.expectedIso));
        CHECK_FALSE(formatted.contains("epoch_micros"));

        // The load-bearing invariant: the printed form re-parses to
        // the identical tree.
        const auto reparsed = ParseQuery(formatted);
        REQUIRE(reparsed.has_value());
        CHECK(source == *reparsed);
    }
}

// `ParseIsoTimestamp` fed its fields straight into the civil-days
// formula, which silently rolls over out-of-range values --
// `2024-13-45` became 2025-02-14. A typo in a bound therefore became
// a wrong filter instead of an error the editor could underline.
TEST_CASE("ParseQuery: out-of-range calendar fields are parse errors", "[query_parser][regression]")
{
    SECTION("rejected")
    {
        const std::array<std::string_view, 7> queries{
            "ts >= 2024-13-01T00:00:00Z", // month 13
            "ts >= 2024-00-01T00:00:00Z", // month 0
            "ts >= 2024-01-32T00:00:00Z", // day 32
            "ts >= 2024-01-00T00:00:00Z", // day 0
            "ts >= 2023-02-29T00:00:00Z", // 2023 is not a leap year
            "ts >= 2024-01-01T25:00:00Z", // hour 25
            "ts >= 2024-01-01T00:61:00Z", // minute 61
        };
        for (const auto &q : queries)
        {
            CAPTURE(q);
            // A rejected timestamp falls back to the numeric branch,
            // which also refuses it -- either way the query must not
            // parse into a Time leaf with a rolled-over bound.
            const auto parsed = ParseQuery(q);
            CHECK_FALSE(parsed.has_value());
        }
    }
    SECTION("genuine leap day still parses")
    {
        const auto expr = ParseOrFail("ts >= 2024-02-29T00:00:00Z");
        const LeafRule *leaf = AsLeaf(expr);
        REQUIRE(leaf != nullptr);
        CHECK(leaf->type == LeafRule::Type::Time);
        REQUIRE(leaf->filterBegin.has_value());
    }
}

// The previous `hour > 24 || minute > 59 || second > 60` check let
// `24:59:59` roll over into the next day's `00:59:59` -- a silent
// off-by-one for anyone who typed a full-second time with `hour==24`.
// ISO-8601 only allows `24:00:00` (start of the next day); everything
// else with `hour == 24` should be a parse error the editor can
// underline.
TEST_CASE(
    "ParseQuery: hour==24 is only valid at midnight", "[query_parser][regression]"
)
{
    SECTION("24:00:00 accepted as end-of-day spelling")
    {
        // `24:00:00Z` on 2024-01-01 is midnight of 2024-01-02.
        const auto expr = ParseOrFail("ts >= 2024-01-01T24:00:00Z");
        const LeafRule *leaf = AsLeaf(expr);
        REQUIRE(leaf != nullptr);
        REQUIRE(leaf->filterBegin.has_value());
        // Same instant as `2024-01-02T00:00:00Z`.
        const auto reference = ParseOrFail("ts >= 2024-01-02T00:00:00Z");
        const LeafRule *refLeaf = AsLeaf(reference);
        REQUIRE(refLeaf != nullptr);
        REQUIRE(refLeaf->filterBegin.has_value());
        CHECK(*leaf->filterBegin == *refLeaf->filterBegin);
    }
    SECTION("24:xx:yy with non-zero minute/second/fraction rejected")
    {
        const std::array<std::string_view, 4> queries{
            "ts >= 2024-01-01T24:00:01Z",     // second != 0
            "ts >= 2024-01-01T24:01:00Z",     // minute != 0
            "ts >= 2024-01-01T24:59:59Z",     // both
            "ts >= 2024-01-01T24:00:00.5Z",   // fractional != 0
        };
        for (const auto &q : queries)
        {
            CAPTURE(q);
            const auto parsed = ParseQuery(q);
            CHECK_FALSE(parsed.has_value());
        }
    }
}

// Mixed-type range bounds (one ISO timestamp, one plain number) used
// to silently drop the number bound: the "Time or Number" branch in
// `FinishRangeUpper` picked Time whenever either side classified as
// timestamp and copied only the `ts*` fields, so
// `col in [2024-01-01T00:00:00Z..42]` became "on or after 2024-01-01"
// with the upper bound gone. Users had no cue.
TEST_CASE(
    "ParseQuery: mixed-type range bounds are parse errors", "[query_parser][regression]"
)
{
    SECTION("timestamp then number")
    {
        const auto parsed = ParseQuery("latency in [2024-01-01T00:00:00Z..42]");
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error().message.contains("numeric or both"));
    }
    SECTION("number then timestamp")
    {
        const auto parsed = ParseQuery("latency in [42..2024-01-01T00:00:00Z]");
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error().message.contains("numeric or both"));
    }
    SECTION("both timestamps still parse")
    {
        const auto parsed = ParseQuery(
            "ts in [2024-01-01T00:00:00Z..2024-02-01T00:00:00Z]"
        );
        REQUIRE(parsed.has_value());
    }
    SECTION("both numeric still parse")
    {
        const auto parsed = ParseQuery("latency in [10..100]");
        REQUIRE(parsed.has_value());
    }
}

// `MirrorSessionStateToConfiguration` wraps Advanced-only subtrees
// (bare `Or` / `Not` roots produced by the Advanced editor) in a
// top-level `And` so the "top-level Leaf children are already in
// `mSimpleLeaves`" invariant holds. `FormatExpression` used to
// print that wrapper faithfully, so `a OR b` came back through the
// editor as `(a OR b)` -- the parens were harmless but visibly
// jittered on every accept/reopen cycle. Peeling a single-child
// `And` at the root removes the noise.
TEST_CASE(
    "FormatExpression: peels a single-child And wrapper at the root",
    "[query_parser][pretty][regression]"
)
{
    SECTION("And([Or([...])]) renders without redundant parens")
    {
        // Build the tree by hand so the wrapping is unambiguous
        // (the parser doesn't produce this shape directly).
        FilterExpression inner = ParseOrFail("service:auth OR level:error");
        std::vector<FilterExpression> children;
        children.push_back(std::move(inner));
        const FilterExpression wrapped = MakeAnd(std::move(children));

        const std::string formatted = FormatExpression(wrapped);
        CAPTURE(formatted);
        CHECK_FALSE(formatted.starts_with("("));
        CHECK_FALSE(formatted.ends_with(")"));
        CHECK(formatted.contains(" OR "));
    }
    SECTION("Semantic round-trip preserved")
    {
        // The AST equality check is the load-bearing invariant --
        // unwrapping is a display concern, not a semantics change.
        const auto original = ParseOrFail("service:auth OR level:error");
        std::vector<FilterExpression> children;
        children.push_back(original);
        const FilterExpression wrapped = MakeAnd(std::move(children));

        const std::string formatted = FormatExpression(wrapped);
        const auto reparsed = ParseQuery(formatted);
        REQUIRE(reparsed.has_value());
        // Reparse yields the bare `Or` (matching what we'd get for
        // the naked query `a OR b`), which equals `original`.
        CHECK(*reparsed == original);
    }
    SECTION("Multi-child And still uses `AND` separators")
    {
        const auto expr = ParseOrFail("service:auth AND level:error");
        const std::string formatted = FormatExpression(expr);
        CHECK(formatted.contains(" AND "));
    }
}

// `col in []` and `col in [..]` used to parse into a leaf with no
// payload. `CompileLeaf` then reported it absent, so the clause
// silently became match-all, and the app's Filters-menu title
// builder tripped a `Q_ASSERT` on the empty payload in Debug. An
// inverted range is the same class of problem from the other side:
// it can never accept a row, so the view goes blank while the editor
// reports "Parsed OK".
TEST_CASE("ParseQuery: payload-less and inverted value lists are errors", "[query_parser][regression]")
{
    SECTION("empty list")
    {
        const auto parsed = ParseQuery("level in []");
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error().message.contains("empty"));
        // Caret lands on the `]`.
        CHECK(parsed.error().offset == 10);
    }
    SECTION("range with neither bound")
    {
        const auto parsed = ParseQuery("latency in [..]");
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error().message.contains("bound"));
    }
    SECTION("inverted numeric range")
    {
        const auto parsed = ParseQuery("latency in [100..10]");
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error().message.contains("below the lower bound"));
    }
    SECTION("inverted time range")
    {
        const auto parsed = ParseQuery("ts in [2024-02-01T00:00:00Z..2024-01-01T00:00:00Z]");
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error().message.contains("below the lower bound"));
    }
    SECTION("single-sided and equal-bound ranges still parse")
    {
        for (const std::string_view q : {"latency in [10..]", "latency in [..100]", "latency in [10..10]"})
        {
            CAPTURE(q);
            const auto parsed = ParseQuery(q);
            CHECK(parsed.has_value());
        }
    }
}

// Regression: `FormatTimestampMicros` did not check the return of
// `gmtime_s` / `strftime`. Windows `gmtime_s` returns `EINVAL` for
// times outside `[0000, 9999]` and leaves the `std::tm` zeroed,
// which `strftime` then rendered as `0000-00-00T00:00:00` -- a
// silently wrong roundtrip. The guarded path falls back to an
// `epoch_micros:<n>` marker instead.
TEST_CASE("FormatExpression: extreme timestamp does not silently render as the epoch", "[query_parser][pretty][regression]")
{
    FilterExpression source;
    LeafRule rule;
    rule.type = LeafRule::Type::Time;
    rule.columnKeys = {"ts"};
    // Well past year 9999 -- Windows `gmtime_s` returns EINVAL for
    // this. POSIX `gmtime_r` clamps or fails depending on libc;
    // either way we want the guarded fallback to fire rather than
    // rendering as the epoch or 0000-00-00. `int64_t::max() / 2`
    // in microseconds sits ~146'000 years past the epoch, well
    // outside anything `gmtime` accepts.
    rule.filterBegin = std::numeric_limits<std::int64_t>::max() / 2;
    source.node = FilterExpression::Leaf{rule};

    const std::string formatted = FormatExpression(source);
    CAPTURE(formatted);
    CHECK_FALSE(formatted.contains("1970-01-01T00:00:00.000000Z"));
    CHECK_FALSE(formatted.contains("0000-00-00T00:00:00"));
}
