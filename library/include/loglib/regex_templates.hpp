#pragma once

#include <span>
#include <string>
#include <string_view>
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
/// `loglib::RegexParser::IsValid` matches each registered template's
/// pattern against the first few non-blank lines of the file; the
/// same lines are used in `test_regex_parser.cpp` to assert that the
/// shipped templates parse the formats they advertise.
///
/// Templates live one-per-file under `library/data/regex_templates/`
/// (embedded into the library at build time) and, for user-supplied
/// shadows / additions, under `<AppDataLocation>/regex_templates/`.
/// Adding a built-in template = drop a new JSON file under the data
/// directory and rebuild. Adding a user template = drop a JSON file
/// in the user directory; no rebuild required.
struct RegexTemplate
{
    /// Stable, human-readable display name (e.g. "Syslog (RFC3164)").
    /// Shown in the GUI template picker. Two templates with the
    /// same `name` collide; a user template silently shadows a
    /// built-in of the same name (mirroring `ThemeControl`).
    std::string name;
    /// PCRE2 pattern with `(?<Name>...)` named capture groups. The
    /// engine compiles it once and shares the compiled code read-only
    /// across all Stage B workers; each worker owns its own
    /// `pcre2_match_data` (see `library/src/parsers/regex_parser.cpp`).
    std::string pattern;
    /// One or more representative lines this pattern is known to
    /// match. Used by `IsValid` for auto-detection (when
    /// `autoDetect` is true) and by unit tests as canonical
    /// fixtures.
    std::vector<std::string> sampleLines;
    /// Whether this template participates in the auto-detection
    /// probe loop. `true` (the default) means "probe this against
    /// every unrecognised file"; `false` keeps it available in the
    /// GUI picker but skips it during auto-detection. Defaulted in
    /// JSON to `true` so existing files without the field keep the
    /// old behaviour.
    bool autoDetect = true;
    /// Auto-detection probe order; lower probes first. Built-ins
    /// curate priorities so more-specific patterns are tried before
    /// looser ones (Combined Log Format before Common Log Format,
    /// etc.). User templates default to a bucket below built-ins so
    /// they never accidentally steal a probe match from a shipped
    /// template. Templates with the same `priority` retain stable
    /// document order. Defaulted in JSON to `100` (user-template
    /// bucket) when absent.
    int priority = 100;
    /// Free-form human-readable description of the format the
    /// template parses. Typical content: a sentence on the format
    /// (what software emits it, what the columns mean, known
    /// edge-cases), optionally followed by an attribution line for
    /// templates ported from an upstream source (e.g.
    /// `Adapted from lnav src/formats/syslog_log.json
    /// (BSD-2-Clause)`). Surfaced in the Regex templates editor
    /// and used by the `THIRD_PARTY_LICENSES.txt` bundler when it
    /// contains a recognisable licence citation. Empty string for
    /// user-written templates that don't need either.
    std::string description;
};

/// Parse a single regex-template JSON document. Throws
/// `std::runtime_error` on parse failure (the message includes
/// glaze's position context). Defaults `autoDetect = true`,
/// `priority = 100`, `description = ""` so older JSON files lacking
/// those fields keep working.
[[nodiscard]] RegexTemplate ParseRegexTemplate(std::string_view content);

/// Serialise @p tmpl to pretty-printed JSON. Throws
/// `std::runtime_error` on encode failure. Used by the GUI
/// "Save as template..." path to atomically write a user template
/// into `<AppDataLocation>/regex_templates/`.
[[nodiscard]] std::string SerializeRegexTemplate(const RegexTemplate &tmpl);

/// Built-in registry of regex templates for common log formats.
///
/// Returned by reference so callers can hold a `span` for the
/// duration of a parse without copying. The registry is constant
/// for the lifetime of the process and is parsed lazily on first
/// call from JSON files embedded into the library at build time
/// (`library/data/regex_templates/*.json`).
///
/// Per-template `description` strings double as upstream-source
/// citations for ports (lnav, logstash-patterns-core, vendor docs).
[[nodiscard]] std::span<const RegexTemplate> BuiltinRegexTemplates() noexcept;

/// Register a snapshot of *extra* templates (user-supplied,
/// app-side) so `RegexParser::IsValid` / `DetectRegexTemplate` see
/// them in the merged probe loop. Replaces any previously
/// registered set (not additive). Pass an empty span to clear.
///
/// The library copies @p extras into an internally-owned vector,
/// so the caller's storage need not outlive the call. Safe to
/// call multiple times (e.g. on a "Reload templates from disk"
/// action); changes take effect on the next probe.
///
/// Built-ins always probe before user templates regardless of
/// `priority`, so a careless user-priority can never steal a probe
/// match from a shipped template; within each tier the merged
/// list is stable-sorted by `priority`.
void SetExtraRegexTemplates(std::span<const RegexTemplate> extras);

/// Find a template (built-in or extra) whose `pattern` matches
/// @p pattern exactly. Used to look up the display name for a
/// persisted `Source::regexPattern` value. Returns `nullptr` when
/// the pattern matches neither a built-in nor a registered user
/// template (i.e. it is custom and unsaved).
[[nodiscard]] const RegexTemplate *FindTemplateByPattern(std::string_view pattern) noexcept;

/// Back-compat alias for `FindTemplateByPattern`. New code should
/// prefer the new name; the old name is kept for one release so
/// downstream consumers that pinned `FindBuiltinByPattern` still
/// compile. Behaves identically (the merged registry has always
/// been the right answer).
[[nodiscard]] const RegexTemplate *FindBuiltinByPattern(std::string_view pattern) noexcept;

} // namespace loglib
