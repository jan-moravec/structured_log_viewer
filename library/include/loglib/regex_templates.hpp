#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace loglib
{

/// A named PCRE2 pattern that splits one log line into columns via
/// named capture groups.
///
/// Patterns use PCRE2 syntax with `(?<Name>...)` named groups.
/// Each named group becomes a `LogConfiguration::Column` whose
/// `keys[0]` is the group name; anonymous groups are ignored.
///
/// `sampleLines` doubles as test fixture and auto-detection data:
/// `RegexParser::IsValid` matches each template's pattern against
/// the first few non-blank lines of the file, and the same lines
/// drive `test_regex_parser.cpp` to assert shipped templates parse
/// the formats they advertise.
///
/// Templates live one-per-file under `library/data/regex_templates/`
/// (embedded into the library at build time) and, for user
/// additions or shadows, under `<AppDataLocation>/regex_templates/`.
/// Adding a built-in template means dropping a new JSON in the
/// data directory and rebuilding; user templates require no build.
struct RegexTemplate
{
    /// Stable display name (e.g. "Syslog (RFC3164)") shown in the
    /// GUI picker. Two templates with the same `name` collide; a
    /// user template silently shadows a built-in of the same name
    /// (mirrors `ThemeControl`).
    std::string name;
    /// PCRE2 pattern with `(?<Name>...)` named capture groups.
    /// Compiled once and shared read-only across Stage B workers;
    /// each worker owns its own `pcre2_match_data` (see
    /// `library/src/parsers/regex_parser.cpp`).
    std::string pattern;
    /// Representative lines the pattern is known to match. Used by
    /// `IsValid` when `autoDetect` is true, and by unit tests as
    /// canonical fixtures.
    std::vector<std::string> sampleLines;
    /// Whether the template joins the auto-detection probe loop.
    /// `true` (default) means "probe this against unrecognised
    /// files"; `false` keeps it in the picker but skips detection.
    /// Defaulted to `true` in JSON so existing files without the
    /// field keep the old behaviour.
    bool autoDetect = true;
    /// Probe order; lower probes first. Built-ins curate priorities
    /// so more-specific patterns beat looser ones (Combined Log
    /// Format before Common Log Format, ...). User templates
    /// default to a bucket below built-ins so they never
    /// accidentally steal a match from a shipped template. Same
    /// `priority` retains document order. Defaulted in JSON to
    /// `100` (user bucket).
    int priority = 100;
    /// Free-form description of the format the template parses:
    /// typically a sentence on the format (what emits it, column
    /// meanings, edge cases) and, for ports, an attribution line
    /// (e.g. `Adapted from lnav src/formats/syslog_log.json
    /// (BSD-2-Clause)`). Surfaced in the Regex templates editor
    /// and scanned by `THIRD_PARTY_LICENSES.txt` for recognisable
    /// licence citations. Empty for user templates that need
    /// neither.
    std::string description;
};

/// Parse a single regex-template JSON document. Throws
/// `std::runtime_error` on parse failure (the message includes
/// glaze's position context). Defaults `autoDetect = true`,
/// `priority = 100`, `description = ""` so older JSON files still
/// load cleanly.
[[nodiscard]] RegexTemplate ParseRegexTemplate(std::string_view content);

/// Serialise @p tmpl to pretty-printed JSON. Throws
/// `std::runtime_error` on encode failure. Used by the GUI's
/// "Save as template..." to atomically write a user template into
/// `<AppDataLocation>/regex_templates/`.
[[nodiscard]] std::string SerializeRegexTemplate(const RegexTemplate &tmpl);

/// Built-in registry of regex templates for common log formats.
///
/// Returned by reference so callers can hold a `span` for a whole
/// parse without copying. The registry is process-lifetime constant
/// and is parsed lazily on first call from JSON files embedded at
/// build time (`library/data/regex_templates/*.json`).
///
/// Per-template `description` strings double as upstream-source
/// citations for ports (lnav, logstash-patterns-core, vendor docs).
[[nodiscard]] std::span<const RegexTemplate> BuiltinRegexTemplates() noexcept;

/// Register a snapshot of *extra* templates (user-supplied,
/// app-side) so `RegexParser::IsValid` / `DetectRegexTemplate` see
/// them in the merged probe loop. Replaces any previously
/// registered set (not additive). Pass an empty span to clear.
///
/// The library copies @p extras into an internal vector, so the
/// caller's storage need not outlive the call. Safe to call
/// repeatedly (e.g. "Reload templates from disk"); changes take
/// effect on the next probe.
///
/// Built-ins always probe before user templates regardless of
/// `priority`, so a careless user priority can never steal a
/// match from a shipped template; within each tier the merged
/// list is stable-sorted by `priority` (lower probes first). The
/// parser-side compile cache preserves this two-tier ordering
/// verbatim — see `CompiledProbeSnapshot` in
/// `library/src/parsers/regex_parser.cpp`.
void SetExtraRegexTemplates(std::span<const RegexTemplate> extras);

/// Find a template (built-in or extra) whose `pattern` matches
/// @p pattern exactly. Used to look up the display name for a
/// persisted `Source::regexPattern`. Returns `std::nullopt` when
/// the pattern matches neither a built-in nor a registered user
/// template (i.e. it is custom and unsaved).
///
/// Returned by value (not by pointer into the registry) so a
/// concurrent `SetExtraRegexTemplates` call cannot invalidate the
/// caller's copy. `RegexTemplate` is a handful of short strings,
/// so the copy is negligible next to the linear scan.
[[nodiscard]] std::optional<RegexTemplate> FindTemplateByPattern(std::string_view pattern);

/// Back-compat alias for `FindTemplateByPattern`. New code should
/// prefer the new name; kept for one release so downstream code
/// pinned on `FindBuiltinByPattern` still compiles. Behaves
/// identically — the merged registry has always been the right
/// answer.
[[nodiscard]] std::optional<RegexTemplate> FindBuiltinByPattern(std::string_view pattern);

} // namespace loglib
