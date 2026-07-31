#pragma once

#include "../log_line.hpp"
#include "../log_parse_sink.hpp"
#include "../log_parser.hpp"
#include "../parser_options.hpp"
#include "../regex_templates.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace loglib
{

class FileLineSource;
class StreamLineSource;

namespace internal
{
struct AdvancedParserOptions;
}

/// Regex-template log parser, PCRE2-backed.
///
/// One PCRE2 pattern with named capture groups splits each line
/// into columns; the group names become column keys. The pattern
/// is supplied externally (no header line as with CSV), via
/// `ParserOptions::configuration->source->regexPattern`. An empty
/// pattern is permitted only for auto-detection probes (`IsValid`),
/// which iterate the built-in registry; calling `ParseStreaming`
/// without one surfaces a single parse error.
///
/// The compiled `pcre2_code*` is shared read-only across Stage B
/// workers; each worker owns its own `pcre2_match_data*`.
/// `pcre2_set_match_limit` / `pcre2_set_depth_limit` contain
/// pathological backtracking on user-supplied patterns; over-limit
/// lines surface as parse errors and the parse keeps going.
///
/// Engine choice: PCRE2+JIT beats RE2 on capture-heavy patterns in
/// rebar's aggregate and runs the upstream lnav / logstash-grok
/// patterns we adopt verbatim. See CONTRIBUTING.md for the full
/// evaluation.
///
/// Known limits:
/// - The pattern itself must match a single line. Records that span
///   multiple physical lines (stack traces, wrapped JSON blobs) are
///   supported when the template opts in via
///   `RegexTemplate::continuationMode`: `Indented` folds any line
///   whose first byte is a space/tab into the previous record's
///   last named capture, and `UntilNextHeader` folds every line
///   until the next line matches the optional `headerAnchor` (or
///   the full pattern when no anchor is set). Templates that leave
///   `continuationMode == None` (the default) parse strictly line-
///   by-line and surface any non-matching line as a parse error.
/// - Custom regex patterns entered directly via
///   `File → Open Network Stream…` always use `continuationMode ==
///   None`; save them as a template first to enable multi-line
///   folding.
/// - The pattern is parser configuration, not file content, so
///   `IsValid` only auto-detects files matching a template from
///   the merged catalog (built-ins ∪ user templates registered
///   via `loglib::SetExtraRegexTemplates`; see
///   `loglib::DetectRegexTemplate`). Files needing a completely
///   custom, unregistered pattern are reachable via
///   `File → Open Network Stream…` (custom pattern field) or by
///   restoring a session whose `LogConfiguration::Source` is
///   already pinned to `Regex` + the desired pattern.
class RegexParser : public LogParser
{
public:
    /// Build a parser that takes its pattern from
    /// `ParserOptions::configuration->source->regexPattern` at parse
    /// time. Used by `LogFactory::Create(Parser::Regex)`.
    RegexParser() = default;

    /// Build a parser pinned to @p pattern, ignoring any pattern
    /// on the configuration snapshot. Used by
    /// `loglib::ParseFile(path)` once auto-detection has picked a
    /// template, and by tests that drive a known pattern through
    /// the pipeline.
    explicit RegexParser(std::string pattern);

    /// Probe the file's first non-blank lines against every
    /// `autoDetect=true` entry in the merged registry (built-ins
    /// + user templates registered via `SetExtraRegexTemplates`).
    /// Returns `true` on the first template whose pattern matches
    /// enough sample lines. Built-ins probe before user templates
    /// so a careless user priority can't steal a match from a
    /// shipped template. Bounded so we don't read large files
    /// just to refuse them. Custom patterns that aren't registered
    /// as a template are not auto-detectable here.
    bool IsValid(const std::filesystem::path &file) const override;

    /// Static-file parse. Pattern is read from
    /// `options.configuration->source->regexPattern` unless the
    /// parser was pinned via the explicit-pattern constructor.
    void ParseStreaming(FileLineSource &source, LogParseSink &sink, ParserOptions options = {}) const override;

    /// Live-tail parse. Same pattern resolution as the static
    /// overload.
    void ParseStreaming(StreamLineSource &source, LogParseSink &sink, ParserOptions options = {}) const override;

    /// Static overload exposing internal tuning knobs (benchmarks
    /// / bisects). Mirrors the equivalents on the other parsers.
    ///
    /// @p explicitPattern: `std::nullopt` means "fall back to
    /// `options.configuration->source->regexPattern`". Any present
    /// value (even an empty `string_view`) overrides the
    /// configuration; an empty override fails closed with the same
    /// "non-empty pattern required" error the streaming overload
    /// emits. This distinguishes "no pattern pinned" from "pinned
    /// to empty", so an explicit `""` cannot silently pick up a
    /// value from the configuration.
    static void ParseStreaming(
        FileLineSource &source,
        LogParseSink &sink,
        const ParserOptions &options,
        internal::AdvancedParserOptions advanced,
        std::optional<std::string_view> explicitPattern = std::nullopt
    );

    /// Best-effort. Regex isn't invertible, so this joins the
    /// named-group values (in pattern-source order, which is also
    /// `RegexParser`'s `KeyIndex` intern order) with a single
    /// space. `Edit -> Copy` uses `RawLine()` for accurate
    /// round-trips; this is a fallback for callers that already
    /// lost the source line.
    std::string ToString(const LogLine &line) const override;

private:
    /// `std::nullopt` means "read pattern from options at parse
    /// time". A non-empty string pins the parser to a specific
    /// pattern.
    std::optional<std::string> mExplicitPattern;
};

/// Probe @p file against the merged auto-detect registry
/// (built-ins + user templates from `SetExtraRegexTemplates`) and
/// return the first matching template, or `std::nullopt` if none
/// match. Same probe `RegexParser::IsValid` runs; exposing it
/// separately lets callers (e.g. `MainWindow::DetectFormatForPath`)
/// capture *which* template matched so they can persist its
/// pattern on `LogConfiguration::Source`.
///
/// Returned by value (not by pointer into the registry) so callers
/// remain safe across a concurrent `SetExtraRegexTemplates` call —
/// the earlier pointer flavour could dangle if the snapshot it
/// aliased dropped its last reference.
[[nodiscard]] std::optional<RegexTemplate> DetectRegexTemplate(const std::filesystem::path &file);

/// Compile-only validation for GUI surfaces (e.g. the Network
/// Stream dialog) to front-load pattern errors before they'd
/// otherwise surface as a single error on the first inbound line.
/// Returns `true` iff @p pattern is non-empty, compiles, *and*
/// contains at least one `(?<Name>...)` named capture group. On
/// failure, @p errorOut receives a user-facing message (matching
/// `ParseStreaming`'s wording). The compiled PCRE2 state is
/// discarded immediately; this is a cheap pre-flight check, not
/// parse setup.
[[nodiscard]] bool ValidateRegexPattern(std::string_view pattern, std::string &errorOut);

/// Compile-check `RegexTemplate::headerAnchor`. Empty is accepted
/// (means "reuse the main pattern for the header probe"). Non-empty
/// must compile with PCRE2 but need NOT declare named capture
/// groups — the anchor is a boolean probe, not a schema. Symmetric
/// with `ValidateRegexPattern` so the editor's Save / Validate can
/// front-load anchor errors the same way it front-loads main-pattern
/// errors. Wording of @p errorOut mirrors the runtime error the
/// streaming pipeline surfaces on a bad anchor
/// ("Header anchor compile failed: ...") so both surfaces read
/// identically.
[[nodiscard]] bool ValidateHeaderAnchor(std::string_view anchor, std::string &errorOut);

/// True iff @p pattern compiles and matches @p line in full
/// (same `PCRE2_ANCHORED | PCRE2_ENDANCHORED` flags as the
/// auto-detect probe). "Matches" therefore means "a `RegexParser`
/// would emit a row for this line". Used by the regex-templates
/// editor to self-test a pattern against its `sampleLines` before
/// saving. Discards the compiled state immediately; intended for
/// one-off interactive checks, not parse hot paths.
///
/// Returns false on compile failure, no-match, or PCRE2 limit
/// overruns. Callers needing the compile error text should call
/// `ValidateRegexPattern` first.
[[nodiscard]] bool PatternMatchesLine(std::string_view pattern, std::string_view line);

} // namespace loglib
