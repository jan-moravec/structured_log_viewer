#pragma once

#include <loglib/filter_expression.hpp>
#include <loglib/log_filter.hpp>

#include <QString>

/// Build a matcher lambda for `CallbackStringRowPredicate`,
/// shared by filter leaves and highlight rules
/// (`HighlightRule::Match` aliases `LeafRule::Match`).
///
/// Pattern is compiled and captured once. `Exactly`/`Contains` take
/// an ASCII fast path (byte compare, no `QString`/`simplified()`
/// round-trip). Regex/Wildcard need Qt's UTF-16 engine but still
/// skip `simplified()` on canonical haystacks. The regex is
/// JIT-primed so parallel workers don't race on a lazy first match.
[[nodiscard]] loglib::CallbackStringRowPredicate::MatchFn MakeStringMatcher(
    const QString &pattern, loglib::LeafRule::Match match
);
