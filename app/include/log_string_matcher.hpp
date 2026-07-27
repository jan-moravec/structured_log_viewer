#pragma once

#include <loglib/filter_expression.hpp>
#include <loglib/log_filter.hpp>

#include <QString>

/// Build a matcher lambda for `loglib::CallbackStringRowPredicate`,
/// shared by session-scope filter leaves and Configuration-scope
/// highlight rules.
///
/// The pattern is compiled once and captured; the inner loop just
/// runs the compare. `Exactly` / `Contains` take an ASCII fast path
/// that byte-compares directly and skips the `QString::fromUtf8` +
/// `simplified()` round-trip when both sides are canonical.
/// Regex / Wildcard need a `QString` (Qt's engine is UTF-16) but
/// still skip the `simplified()` pass on canonical haystacks.
///
/// The regex is JIT-primed eagerly so captured copies don't race on
/// a lazy first `match()` from parallel filter workers.
///
/// `LeafRule::Match` is the canonical match enum -- filter and
/// highlight-rule paths both consume it (highlight rules alias it
/// as `HighlightRule::Match`).
[[nodiscard]] loglib::CallbackStringRowPredicate::MatchFn MakeStringMatcher(
    const QString &pattern, loglib::LeafRule::Match match
);
