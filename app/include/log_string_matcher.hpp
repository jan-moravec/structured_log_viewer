#pragma once

#include <loglib/log_configuration.hpp>
#include <loglib/log_filter.hpp>

#include <QString>

/// Build a Qt-flavoured matcher lambda suitable for
/// `loglib::CallbackStringRowPredicate`.
///
/// Extracted from `main_window.cpp` so both the session-scope filter
/// pipeline and the Configuration-scope highlight rule set share a
/// single implementation of "compile a Qt regex / wildcard / literal
/// needle to a per-row lambda". The two callers use structurally
/// identical `Match` enums; the highlight-rule overload maps its
/// enum onto `LogFilter::Match` at call time.
///
/// Captures the compiled regex / needle once so the inner loop
/// avoids per-row recompilation. `Exactly` / `Contains` get an ASCII
/// fast path: when both pattern and haystack pass
/// `LogModel::IsSingleLineAsciiTrim` the matcher byte-compares
/// directly and skips the `QString::fromUtf8` + `simplified`
/// round-trip. Regex / wildcard still need a `QString` (Qt's regex
/// engine is UTF-16) but skip the `replace` + `simplified` passes
/// when the haystack is canonical.
///
/// Regex is JIT-compiled eagerly at build time so each captured copy
/// doesn't re-JIT lazily on its first `match()` (the real
/// thread-safety footgun for `QRegularExpression`).
[[nodiscard]] loglib::CallbackStringRowPredicate::MatchFn MakeStringMatcher(
    const QString &pattern, loglib::LogConfiguration::LogFilter::Match match
);

/// Same, for the Configuration-scope `HighlightRule::Match`. The
/// two enums have identical enumerator names and identical semantics
/// (mirrored on purpose so this factory can back both callers); the
/// overload picks the right cast at compile time.
[[nodiscard]] loglib::CallbackStringRowPredicate::MatchFn MakeStringMatcher(
    const QString &pattern, loglib::LogConfiguration::HighlightRule::Match match
);
