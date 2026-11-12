// The corpus expected flags are the Qt oracle (`apptest_log_string_matcher`).
// The PCRE2 case below is `loglib::MakeStringMatcher`. `*` and `?` match
// `/` and `\`, which Qt's default wildcard conversion does not.

#include "string_matcher_corpus.hpp"

#include <loglib/query/log_string_matcher.hpp>

#include <catch2/catch_all.hpp>

#include <cctype>
#include <set>
#include <string>
#include <string_view>

TEST_CASE("string matcher differential corpus covers the required axes", "[string_matcher][corpus]")
{
    const std::vector<StringMatcherCase> &cases = StringMatcherCorpus();
    REQUIRE(cases.size() >= 60);

    std::set<std::string> ids;
    bool sawExactly = false;
    bool sawContains = false;
    bool sawRegex = false;
    bool sawWildcard = false;
    for (const StringMatcherCase &entry : cases)
    {
        INFO(entry.id);
        REQUIRE(ids.insert(std::string(entry.id)).second);
        switch (entry.match)
        {
        case loglib::LeafRule::Match::Exactly:
            sawExactly = true;
            break;
        case loglib::LeafRule::Match::Contains:
            sawContains = true;
            break;
        case loglib::LeafRule::Match::RegularExpression:
            sawRegex = true;
            break;
        case loglib::LeafRule::Match::Wildcard:
            sawWildcard = true;
            break;
        }
    }
    CHECK(sawExactly);
    CHECK(sawContains);
    CHECK(sawRegex);
    CHECK(sawWildcard);

    const std::set<std::string_view> required = {
        "exactly-utf8-equal",
        "exactly-nul",
        "exactly-invalid-utf8-collapses",
        "exactly-leading-space",
        "exactly-interior-collapse",
        "exactly-empty-both",
        "regex-anchor-start",
        "regex-alternation",
        "regex-class",
        "wildcard-star",
        "wildcard-question",
        "wildcard-class",
        "wildcard-star-not-slash",
        "exactly-case",
        "contains-case",
        "regex-case",
        "wildcard-case",
    };
    for (const std::string_view id : required)
    {
        INFO(id);
        CHECK(ids.contains(std::string(id)));
    }
}

namespace
{

/// `*` → `.*` and `?` → `.`, so these three Qt "path wildcard" cases match.
bool Pcre2ExpectsMatch(const StringMatcherCase &entry)
{
    if (entry.id == "wildcard-star-not-slash" || entry.id == "wildcard-star-not-backslash" ||
        entry.id == "wildcard-question-not-slash")
    {
        return true;
    }
    return entry.expected;
}

} // namespace

TEST_CASE("PCRE2 string matcher matches the Qt corpus except separator wildcards", "[string_matcher]")
{
    for (const StringMatcherCase &entry : StringMatcherCorpus())
    {
        INFO(entry.id);
        const auto matcher = loglib::MakeStringMatcher(entry.pattern, entry.match);
        const bool actual = matcher(entry.haystack);
        if (entry.id == "wildcard-star-not-slash" || entry.id == "wildcard-star-not-backslash" ||
            entry.id == "wildcard-question-not-slash")
        {
            CHECK(entry.expected == false);
        }
        CHECK(actual == Pcre2ExpectsMatch(entry));
    }
}

namespace
{

// The slow, plain reference: a regular expression on the simplified cell, which is how every match type was evaluated
// before Contains, Exactly and star-text wildcards got their own paths.
bool ReferenceMatch(std::string_view regex, std::string_view haystack)
{
    return loglib::MakeStringMatcher(regex, loglib::LeafRule::Match::RegularExpression)(haystack);
}

// The text as a regular expression that matches it literally: every ASCII punctuation or space byte gets a backslash.
std::string EscapeLiteral(std::string_view text)
{
    std::string out;
    for (const char ch : text)
    {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte > 0x20 && byte < 0x7F && std::ispunct(byte) != 0)
        {
            out.push_back('\\');
        }
        else if (byte == 0x20)
        {
            out.append("\\ ");
            continue;
        }
        out.push_back(ch);
    }
    return out;
}

} // namespace

TEST_CASE(
    "Contains, Exactly and star-text wildcard agree with a regular expression on cells that need simplifying",
    "[string_matcher][fast_path]"
)
{
    using Match = loglib::LeafRule::Match;
    const std::vector<std::string> haystacks = {
        "",
        "abc",
        "user-id=42",
        "  user-id=42  ",
        "user-id=42\t",
        "request  handled   for  user",
        "request handled for user",
        "timeout\tafter 5",
        "timeout after",
        "timeout  after",
        " timeout after ",
        "a\xC2\xA0 b",
        "a\xC2\xA0"
        "b",
        "a\xE2\x80\xA8"
        "b",
        "a\xE2\x80\xA8 b",
        "bad\xFF"
        "x",
        "na\xC3\xAFve",
        "na\xC3\xAFve user",
        "\xE2\x82",
        "line\nbreak",
        "x \r\n y",
        "foo bar",
        "foo\xA0"
        "bar",
        " foo ",
        "foo",
        "food",
        "\tfoo ",
        "a b",
        "a\x01"
        "b",
        "a  b",
        "a\tb",
        "\x7F"
        "x",
    };
    // Needles that are already in simplified form, in every class the fast paths tell apart: a plain word, text with
    // single inner spaces, an edge space, non-ASCII text, a control byte and the empty needle.
    const std::vector<std::string> needles = {
        "user-id",
        "id=42",
        "timeout after",
        "a b",
        "x",
        "",
        " foo ",
        "foo",
        "foo ",
        " foo",
        "na\xC3\xAFve",
        "\xC3\xA9",
        "handled for user",
        "after",
        "b",
        "a\x01"
        "b",
        "na\xC3\xAFve user",
        "foo bar",
        "abc",
    };
    for (const std::string &needle : needles)
    {
        const std::string quoted = EscapeLiteral(needle);
        const auto contains = loglib::MakeStringMatcher(needle, Match::Contains);
        const bool hasEdgeSpace = !needle.empty() && (needle.front() == ' ' || needle.back() == ' ');
        const auto exactly = loglib::MakeStringMatcher(needle, Match::Exactly);
        const auto wildcard = loglib::MakeStringMatcher("*" + needle + "*", Match::Wildcard);
        const bool plain = needle.find_first_of("*?[") == std::string::npos;
        for (const std::string &haystack : haystacks)
        {
            INFO("needle '" << needle << "' haystack '" << haystack << "'");
            CHECK(contains(haystack) == ReferenceMatch(quoted, haystack));
            if (!hasEdgeSpace)
            {
                CHECK(exactly(haystack) == ReferenceMatch("\\A" + quoted + "\\z", haystack));
            }
            if (plain)
            {
                CHECK(wildcard(haystack) == ReferenceMatch("\\A(?:.*" + quoted + ".*)\\z", haystack));
            }
        }
    }
}
