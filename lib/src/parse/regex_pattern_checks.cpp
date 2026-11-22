#include "loglib/parse/parsers/regex_parser.hpp"

#include "loglib/core/internal/pcre2_support.hpp"
#include "loglib/parse/internal/regex_compiled_pattern.hpp"

#include <string>
#include <string_view>

namespace loglib
{

using internal::CompiledPattern;
using internal::Pcre2MatchDataPtr;

bool ValidateRegexPattern(std::string_view pattern, std::string &errorOut)
{
    if (pattern.empty())
    {
        errorOut = "Regex parser requires a non-empty pattern.";
        return false;
    }
    CompiledPattern compiled;
    if (!compiled.Compile(pattern, errorOut))
    {
        return false;
    }
    if (!compiled.HasNamedGroups())
    {
        errorOut = "Regex pattern has no named capture groups; nothing to put in columns. Use `(?<Name>...)`.";
        return false;
    }
    return true;
}

bool ValidateHeaderAnchor(std::string_view anchor, std::string &errorOut)
{
    // Success is represented by an empty error string.
    errorOut.clear();
    if (anchor.empty())
    {
        return true;
    }
    // A boolean header probe does not define a capture schema.
    CompiledPattern compiled;
    std::string compileErr;
    if (!compiled.Compile(anchor, compileErr))
    {
        errorOut = "Header anchor compile failed: " + compileErr;
        return false;
    }
    return true;
}

bool PatternMatchesLine(std::string_view pattern, std::string_view line)
{
    if (pattern.empty())
    {
        return false;
    }
    CompiledPattern compiled;
    std::string ignoredError;
    if (!compiled.Compile(pattern, ignoredError))
    {
        return false;
    }
    const Pcre2MatchDataPtr matchData = compiled.NewMatchData();
    if (!matchData)
    {
        return false;
    }
    // PCRE2_ANCHORED | PCRE2_ENDANCHORED mirrors the probe loop
    // (see MatchesWholeLine in regex_template_catalog.cpp): a partial mid-line
    // match isn't what a RegexParser would emit for this line,
    // so it should fail the self-test too. The two callers
    // **must** stay in sync — loosen one, loosen the other, or
    // Validate would pass while the probe fails (or vice-versa).
    const int rc = pcre2_match(
        compiled.Code(),
        reinterpret_cast<PCRE2_SPTR>(line.data()),
        line.size(),
        /*startoffset*/ 0,
        PCRE2_ANCHORED | PCRE2_ENDANCHORED,
        matchData.get(),
        compiled.Context()
    );
    return rc >= 0;
}

} // namespace loglib
