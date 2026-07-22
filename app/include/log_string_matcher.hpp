#pragma once

#include <loglib/log_configuration.hpp>
#include <loglib/log_filter.hpp>

#include <QString>

/// Build a matcher lambda for `loglib::CallbackStringRowPredicate`,
/// shared by session-scope filters and Configuration-scope highlight
/// rules.
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
[[nodiscard]] loglib::CallbackStringRowPredicate::MatchFn MakeStringMatcher(
    const QString &pattern, loglib::LogConfiguration::LogFilter::Match match
);

/// Overload for `HighlightRule::Match`; casts to `LogFilter::Match`
/// (the two enums are pinned identical by static_assert).
[[nodiscard]] loglib::CallbackStringRowPredicate::MatchFn MakeStringMatcher(
    const QString &pattern, loglib::LogConfiguration::HighlightRule::Match match
);
