#pragma once

#include <span>
#include <string>
#include <vector>

namespace loglib
{

/// A named PCRE2 pattern that splits one log line into columns via
/// named capture groups.
///
/// Patterns are PCRE2 syntax (`(?<Name>...)` named groups). Each
/// named group becomes a `LogConfiguration::Column`; the group name
/// is the column's `keys[0]`. Anonymous groups are ignored.
///
/// `sampleLines` doubles as test fixture and auto-detection data:
/// `loglib::RegexParser::IsValid` matches each builtin template's
/// pattern against the first few non-blank lines of the file; the
/// same lines are used in `test_regex_parser.cpp` to assert that the
/// builtins parse the formats they advertise.
///
/// Adding a template: append at the bottom of `BuiltinRegexTemplates`
/// (auto-detection probe order is registry order; existing slots are
/// stable so persisted `Source::regexPattern` values keep resolving
/// to the same display name).
struct RegexTemplate
{
    /// Stable, human-readable display name (e.g. "Syslog (RFC3164)").
    /// Shown in the GUI template picker.
    std::string name;
    /// PCRE2 pattern with `(?<Name>...)` named capture groups. The
    /// engine compiles it once and shares the compiled code read-only
    /// across all Stage B workers; each worker owns its own
    /// `pcre2_match_data` (see `library/src/parsers/regex_parser.cpp`).
    std::string pattern;
    /// One or more representative lines this pattern is known to
    /// match. Used by `IsValid` for auto-detection and by unit
    /// tests as canonical fixtures.
    std::vector<std::string> sampleLines;
};

/// Built-in registry of regex templates for common log formats.
///
/// Probe order = registry order (top wins). Returned by reference so
/// callers can hold a `span` for the duration of a parse without
/// copying. The registry is constant for the lifetime of the
/// process.
///
/// Source attribution lives next to each pattern in
/// `library/src/regex_templates.cpp`.
[[nodiscard]] std::span<const RegexTemplate> BuiltinRegexTemplates() noexcept;

/// Find a builtin template whose `pattern` matches @p pattern
/// exactly. Used to look up the display name for a persisted
/// `Source::regexPattern` value. Returns `nullptr` when the
/// pattern is custom (user-supplied) and not a builtin.
[[nodiscard]] const RegexTemplate *FindBuiltinByPattern(std::string_view pattern) noexcept;

} // namespace loglib
