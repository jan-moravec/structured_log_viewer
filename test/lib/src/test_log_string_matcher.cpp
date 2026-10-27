// The corpus expected flags are the Qt oracle (`apptest_log_string_matcher`).
// The PCRE2 case below is `loglib::MakeStringMatcher`. `*` and `?` match
// `/` and `\`, which Qt's default wildcard conversion does not.

#include "string_matcher_corpus.hpp"

#include <loglib/log_string_matcher.hpp>

#include <catch2/catch_all.hpp>

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
