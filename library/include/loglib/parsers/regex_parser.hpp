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
/// One PCRE2 pattern with named capture groups splits each line into
/// columns; the group names become column keys (and default headers).
/// The pattern is supplied externally — there is no header line in
/// the file as with CSV — and flows in through
/// `ParserOptions::configuration->source->regexPattern`. A default
/// empty pattern is permitted only for auto-detection probes
/// (`IsValid`), which iterate the built-in registry; calling
/// `ParseStreaming` with no pattern surfaces a single parse error
/// rather than attempting to match.
///
/// The compiled `pcre2_code*` is shared read-only across Stage B
/// workers; each worker owns its own `pcre2_match_data*` (the
/// per-match scratch). `pcre2_set_match_limit` /
/// `pcre2_set_depth_limit` contain pathological backtracking on
/// user-supplied patterns; over-limit lines surface as parse errors
/// and the parse keeps going.
///
/// Engine choice: PCRE2+JIT beats RE2 on capture-heavy patterns in
/// rebar's aggregate, and runs the upstream lnav / logstash-grok
/// patterns we adopt verbatim. See the regex-template plan and
/// CONTRIBUTING.md for the full evaluation.
///
/// Known limits:
/// - Patterns must match a single line. Multi-line records (stack
///   traces) are out of scope; they surface as one parse error per
///   line in the trailer.
/// - The pattern itself is parser configuration, not file content,
///   so `IsValid` only auto-detects files that match a *built-in*
///   template (see `loglib::DetectRegexTemplate`). Files that need a
///   custom user pattern are reachable today through
///   `File → Open Network Stream…` (custom pattern field) or by
///   restoring a saved session whose `LogConfiguration::Source` is
///   already pinned to `Regex` + the desired pattern.
class RegexParser : public LogParser
{
public:
    /// Build a parser that takes its pattern from
    /// `ParserOptions::configuration->source->regexPattern` at parse
    /// time. Used by `LogFactory::Create(Parser::Regex)`.
    RegexParser() = default;

    /// Build a parser pinned to @p pattern, ignoring any pattern on
    /// the configuration snapshot. Used by `loglib::ParseFile(path)`
    /// once auto-detection has identified the matching template,
    /// and by tests that want to drive a known pattern through the
    /// pipeline.
    explicit RegexParser(std::string pattern);

    /// Probes the file's first non-blank lines against every entry
    /// in `BuiltinRegexTemplates()`. Returns `true` on the first
    /// template whose pattern matches enough sample lines. The
    /// probe is bounded so we don't read large files just to refuse
    /// them. Custom patterns are not auto-detectable here.
    bool IsValid(const std::filesystem::path &file) const override;

    /// Static-file parse. Pattern is read from
    /// `options.configuration->source->regexPattern` unless the
    /// parser was pinned via the explicit-pattern constructor.
    void ParseStreaming(FileLineSource &source, LogParseSink &sink, ParserOptions options = {}) const override;

    /// Live-tail parse. Same pattern resolution as the static
    /// overload.
    void ParseStreaming(StreamLineSource &source, LogParseSink &sink, ParserOptions options = {}) const override;

    /// Static overload exposing internal tuning knobs (benchmarks /
    /// bisects). Mirrors the equivalent on the other parsers.
    ///
    /// @p explicitPattern: `std::nullopt` means "fall back to
    /// `options.configuration->source->regexPattern`". A present
    /// value (even an empty `string_view`) overrides the
    /// configuration; an empty override fails closed with the same
    /// "non-empty pattern required" error the `StreamLineSource`
    /// overload emits. This distinguishes "no pattern pinned" from
    /// "pinned to an empty pattern" so a parser explicitly
    /// constructed with `""` cannot silently pick up a value from
    /// the configuration snapshot.
    static void ParseStreaming(
        FileLineSource &source,
        LogParseSink &sink,
        const ParserOptions &options,
        internal::AdvancedParserOptions advanced,
        std::optional<std::string_view> explicitPattern = std::nullopt
    );

    /// Best-effort. Regex is not invertible, so we join the named
    /// group values (in pattern-source order — the order the user
    /// wrote the `(?<Name>...)` groups, which is also `RegexParser`'s
    /// `KeyIndex` intern order) with a single space. `Edit -> Copy`
    /// uses `RawLine()` for accurate round-trips; this is a fallback
    /// for callers that already lost the source line.
    std::string ToString(const LogLine &line) const override;

private:
    /// `std::nullopt` means "read pattern from options at parse time".
    /// A non-empty string here pins the parser to a specific pattern.
    std::optional<std::string> mExplicitPattern;
};

/// Probe @p file against `BuiltinRegexTemplates()` and return the
/// first matching template, or `nullptr` if none match. The same
/// probe `RegexParser::IsValid` runs; exposing it separately lets
/// callers (e.g. `MainWindow::DetectFormatForPath`) capture *which*
/// template matched so they can persist its pattern on the
/// `LogConfiguration::Source`.
[[nodiscard]] const RegexTemplate *DetectRegexTemplate(const std::filesystem::path &file);

/// Compile-only validation used by GUI surfaces (e.g. the Network
/// Stream dialog) to front-load pattern errors before they surface
/// as a single error on the first inbound line. Returns
/// `true` iff @p pattern is non-empty, compiles successfully, *and*
/// contains at least one `(?<Name>...)` named capture group. On
/// failure, @p errorOut is populated with a user-facing message
/// (mirroring what `ParseStreaming` would emit). The compiled
/// PCRE2 state is discarded immediately; this is a cheap pre-flight
/// check, not a parse setup.
[[nodiscard]] bool ValidateRegexPattern(std::string_view pattern, std::string &errorOut);

} // namespace loglib
