/**
 * @brief Builds string matchers for filter leaves.
 *
 * A matcher compares a cell with a user pattern after normalizing whitespace. It supports exact, contains, wildcard and
 * regular expression matches. Patterns that need it are compiled once, with JIT where the platform allows. The returned
 * matcher is safe to call from several threads. StringPatternProblem reports why a pattern would match nothing, so the
 * interface can show the error.
 *
 * This is part of the query module.
 */
#pragma once

#include "loglib/query/log_filter.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace loglib
{

/**
 * @brief Builds the text test for one string leaf.
 *
 * The text being tested is normalized first: spaces at both ends are removed and every run of whitespace inside becomes
 * one space. Exactly and Contains skip that work when both texts are plain ASCII that is already in that form. The
 * pattern of Exactly and Wildcard is normalized the same way. The pattern of Contains also has its runs of whitespace
 * collapsed, but keeps a space at either end, so that " foo " finds a whole word. A regular expression is used as
 * written.
 *
 * Regular expressions and wildcards are compiled once, JIT-compiled where the platform allows, and warmed up. An
 * invalid pattern matches nothing. A wildcard is turned into a regular expression: special characters are escaped, *
 * becomes
 * .* and ? becomes a dot, [...] classes are kept, and the pattern must match the whole text. Both * and ? also match
 * slashes and backslashes.
 *
 * The returned function can be called from several threads.
 */
[[nodiscard]] CallbackStringRowPredicate::MatchFn MakeStringMatcher(std::string_view pattern, LeafRule::Match match);

/** @brief Says why MakeStringMatcher would match nothing for this pattern. Returns nullopt if the pattern compiles. */
[[nodiscard]] std::optional<std::string> StringPatternProblem(std::string_view pattern, LeafRule::Match match);

} // namespace loglib
