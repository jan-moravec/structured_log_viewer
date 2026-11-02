#pragma once

#include "loglib/log_filter.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace loglib
{

/**
 * @brief Builds a predicate for one string leaf.
 *
 * The haystack is compared after single-line whitespace normalisation:
 * ends are trimmed and each internal whitespace run becomes one space.
 * `Exactly` and `Contains` skip that work when both sides are already
 * canonical ASCII. `Exactly` and `Wildcard` patterns get the same
 * normalisation. `Contains` patterns also collapse whitespace runs but
 * keep one space at an end, so `" foo "` finds a whole word.
 * `RegularExpression` patterns are used as written.
 *
 * `RegularExpression` and `Wildcard` compile once, JIT-compile when the
 * platform allows, and prime the JIT. An invalid pattern matches
 * nothing. `Wildcard` escapes regex metacharacters, maps `*` to `.*`
 * and `?` to `.`, copies `[...]` classes through, and anchors the
 * result to the whole haystack. `*` and `?` match `/` and `\`.
 *
 * The returned function is safe to call from several threads.
 *
 * @param pattern UTF-8 pattern.
 * @param match Comparison to perform.
 * @return Predicate over a UTF-8 haystack.
 */
[[nodiscard]] CallbackStringRowPredicate::MatchFn MakeStringMatcher(std::string_view pattern, LeafRule::Match match);

/**
 * @brief Explains why `MakeStringMatcher` would match nothing for @p pattern.
 *
 * @param pattern UTF-8 pattern.
 * @param match Comparison to perform.
 * @return A message for an invalid pattern, or `std::nullopt` when it compiles.
 */
[[nodiscard]] std::optional<std::string> StringPatternProblem(std::string_view pattern, LeafRule::Match match);

} // namespace loglib
