/**
 * @brief Parser driven by one regular expression.
 *
 * A PCRE2 pattern with named capture groups splits each line into fields, and the group names become the keys. The
 * pattern comes from the constructor or from the configuration. Templates can also fold continuation lines into the
 * previous record. It supports both the parallel file pipeline and the live-stream path.
 *
 * This is part of the parse module.
 */
#pragma once

#include "loglib/core/log_line.hpp"
#include "loglib/parse/log_parse_sink.hpp"
#include "loglib/parse/log_parser.hpp"
#include "loglib/parse/parser_options.hpp"
#include "loglib/parse/regex_templates.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace loglib
{

class FileLineSource;
class StreamLineSource;

/**
 * @brief Regex-template log parser, PCRE2-backed.
 *
 * One PCRE2 pattern with named capture groups splits each line
 * into columns; the group names become column keys. The pattern
 * is supplied externally (no header line as with CSV), via
 * ParserOptions::configuration->source->regexPattern. An empty
 * pattern is permitted only for auto-detection probes (IsValid),
 * which iterate the built-in registry; calling ParseStreaming
 * without one surfaces a single parse error.
 *
 * The compiled pcre2_code* is shared read-only across Stage B
 * workers; each worker owns its own pcre2_match_data*.
 * pcre2_set_match_limit / pcre2_set_depth_limit contain
 * pathological backtracking on user-supplied patterns; over-limit
 * lines surface as parse errors and the parse keeps going.
 *
 * Engine choice: PCRE2+JIT beats RE2 on capture-heavy patterns in
 * rebar's aggregate and runs the upstream lnav / logstash-grok
 * patterns we adopt verbatim. See CONTRIBUTING.md for the full
 * evaluation.
 *
 * Known limits:
 * - The pattern matches one physical line. Registered templates may
 *   fold additional lines according to continuationMode; unregistered
 *   patterns remain strictly line-oriented.
 * - The pattern is parser configuration, not file content, so
 *   IsValid only auto-detects files matching a template from
 *   the merged catalog (built-ins ∪ user templates registered
 *   via loglib::SetExtraRegexTemplates; see
 *   loglib::DetectRegexTemplate). Files needing a completely
 *   custom, unregistered pattern are reachable via
 *   File → Open Network Stream… (custom pattern field) or by
 *   restoring a session whose Source is
 *   already pinned to Regex + the desired pattern.
 */
class RegexParser : public LogParser
{
public:
    /**
     * @brief Build a parser that takes its pattern from
     * ParserOptions::configuration->source->regexPattern at parse
     * time.
     */
    RegexParser() = default;

    /**
     * @brief Build a parser pinned to @p pattern, ignoring any pattern
     * on the configuration snapshot. Used by
     * loglib::ParseFile(path) once auto-detection has picked a
     * template, and by tests that drive a known pattern through
     * the pipeline.
     */
    explicit RegexParser(std::string pattern);

    /**
     * @brief Probe @p sniffBuffer's first non-blank lines against every
     * autoDetect=true entry in the merged registry (built-ins
     * + user templates registered via SetExtraRegexTemplates).
     * Returns true on the first template whose pattern matches
     * enough sample lines. Built-ins probe before user templates
     * so a careless user priority can't steal a match from a
     * shipped template. Bounded by the caller's supplied buffer
     * (PROBE_BYTES_BUDGET when called via the file shim), so we
     * don't read large files just to refuse them. Custom patterns
     * that aren't registered as a template are not auto-detectable
     * here.
     */
    bool IsValidBytes(std::string_view sniffBuffer) const override;

    /**
     * @brief Static-file parse. Pattern is read from
     * options.configuration->source->regexPattern unless the
     * parser was pinned via the explicit-pattern constructor.
     */
    void ParseStreaming(FileLineSource &source, LogParseSink &sink, ParserOptions options = {}) const override;

    /**
     * @brief Live-tail parse. Same pattern resolution as the static
     * overload.
     */
    void ParseStreaming(StreamLineSource &source, LogParseSink &sink, ParserOptions options = {}) const override;

private:
    /**
     * @brief std::nullopt means "read pattern from options at parse
     * time". A non-empty string pins the parser to a specific
     * pattern.
     */
    std::optional<std::string> mExplicitPattern;
};

/**
 * @brief Probe @p file against the merged auto-detect registry
 * (built-ins + user templates from SetExtraRegexTemplates) and
 * return the first matching template, or std::nullopt if none
 * match. Same probe RegexParser::IsValid runs; exposing it
 * separately lets callers (e.g. an application that persists the
 * pattern) capture *which* template matched so they can persist its
 * pattern on Source.
 *
 * Returned by value (not by pointer into the registry) so callers
 * remain safe across a concurrent SetExtraRegexTemplates call. A
 * pointer into the registry could dangle if the snapshot it aliased
 * dropped its last reference.
 */
[[nodiscard]] std::optional<RegexTemplate> DetectRegexTemplate(const std::filesystem::path &file);

/**
 * @brief Byte-buffer variant used by AutoDetectParser (network
 * streams) and by stdin auto-detect. Same probe semantics as the
 * file overload. At most PROBE_BYTES_BUDGET bytes are scanned.
 * Named distinctly from DetectRegexTemplate so a std::string
 * argument never resolves here by accident.
 */
[[nodiscard]] std::optional<RegexTemplate> DetectRegexTemplateFromBytes(std::string_view sniffBuffer);

/**
 * @brief Compile-only validation for GUI surfaces (e.g. the Network
 * Stream dialog) to front-load pattern errors before they'd
 * otherwise surface as a single error on the first inbound line.
 * Returns true iff @p pattern is non-empty, compiles, *and*
 * contains at least one (?<Name>...) named capture group. On
 * failure, @p errorOut receives a user-facing message (matching
 * ParseStreaming's wording). The compiled PCRE2 state is
 * discarded immediately; this is a cheap pre-flight check, not
 * parse setup.
 */
[[nodiscard]] bool ValidateRegexPattern(std::string_view pattern, std::string &errorOut);

/**
 * @brief Validate a header probe. Empty reuses the main pattern; non-empty
 * probes need to compile but do not require named captures.
 */
[[nodiscard]] bool ValidateHeaderAnchor(std::string_view anchor, std::string &errorOut);

/**
 * @brief True iff @p pattern compiles and matches @p line in full
 * (same PCRE2_ANCHORED | PCRE2_ENDANCHORED flags as the
 * auto-detect probe). "Matches" therefore means "a RegexParser
 * would emit a row for this line". Used by the regex-templates
 * editor to self-test a pattern against its sampleLines before
 * saving. Discards the compiled state immediately; intended for
 * one-off interactive checks, not parse hot paths.
 *
 * Returns false on compile failure, no-match, or PCRE2 limit
 * overruns. Callers needing the compile error text should call
 * ValidateRegexPattern first.
 */
[[nodiscard]] bool PatternMatchesLine(std::string_view pattern, std::string_view line);

} // namespace loglib
