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
 * @brief Parses lines with a PCRE2 pattern that has named groups. The group names become the column keys.
 *
 * The pattern is not read from the file, as a CSV header is. It comes from
 * ParserOptions::configuration->source->regexPattern or from the constructor. An empty pattern is allowed only for
 * detection probes, which go through the registry of templates. Calling ParseStreaming without a pattern reports one
 * parse error.
 *
 * The compiled pattern is shared by all worker threads, and each thread has its own match data. Match and depth limits
 * contain patterns that backtrack too much. A line that goes over a limit becomes a parse error and parsing goes on.
 *
 * PCRE2 with JIT was chosen over RE2 because it was faster on patterns with many captures and it runs the patterns of
 * lnav and logstash-grok unchanged. CONTRIBUTING.md has the evaluation.
 *
 * Limits:
 * - A pattern matches one physical line. A registered template can join more lines according to its continuationMode.
 *   A pattern that is not registered stays line by line.
 * - The pattern is configuration and not part of the file, so IsValid only detects files that match a template in the
 *   catalog: the built-in templates plus those registered with SetExtraRegexTemplates, as DetectRegexTemplate finds
 *   them. A file that needs a custom pattern that is not registered can be opened as a network stream with a custom
 *   pattern, or by restoring a session whose source is already set to the regex format with that pattern.
 */
class RegexParser : public LogParser
{
public:
    /**
     * @brief Creates a parser that takes its pattern from ParserOptions::configuration->source->regexPattern when it
     * parses.
     */
    RegexParser() = default;

    /**
     * @brief Creates a parser that always uses the given pattern and ignores the one in the configuration.
     *
     * ParseFile uses it after detection has chosen a template, and tests use it to run a known pattern.
     */
    explicit RegexParser(std::string pattern);

    /**
     * @brief Says whether the first non-blank lines of the sample match a template with auto-detection turned on.
     *
     * The templates are tried in order, built-in ones first and then those registered with SetExtraRegexTemplates, so a
     * user priority cannot take a match from a shipped template. The result is true for the first template whose
     * pattern matches enough sample lines. Only the sample is read, up to PROBE_BYTES_BUDGET bytes through the file
     * overload, so large files are not read just to be refused. A custom pattern that is not a registered template is
     * not detected.
     */
    bool IsValidBytes(std::string_view sniffBuffer) const override;

    /**
     * @brief Parses a static file. The pattern comes from options.configuration->source->regexPattern, unless the
     * parser was created with a pattern.
     */
    void ParseStreaming(FileLineSource &source, LogParseSink &sink, ParserOptions options = {}) const override;

    /** @brief Parses a live stream. The pattern is chosen as for a static file. */
    void ParseStreaming(StreamLineSource &source, LogParseSink &sink, ParserOptions options = {}) const override;

private:
    /** @brief The pattern given to the constructor. Empty means the pattern is read from the options when parsing. */
    std::optional<std::string> mExplicitPattern;
};

/**
 * @brief Finds the first template, built-in or registered, that matches the start of a file. Returns nullopt if none
 * does.
 *
 * It is the same check RegexParser::IsValid runs. It is separate so that a caller which saves the pattern, with the
 * source, can learn which template matched. The template is returned by value, so a concurrent call to
 * SetExtraRegexTemplates cannot leave the caller with a dangling pointer.
 */
[[nodiscard]] std::optional<RegexTemplate> DetectRegexTemplate(const std::filesystem::path &file);

/**
 * @brief Like DetectRegexTemplate, for a sample in memory. AutoDetectParser and the detection of standard input use it.
 *
 * At most PROBE_BYTES_BUDGET bytes are scanned. The name differs from DetectRegexTemplate so that a std::string
 * argument cannot select this overload by accident.
 */
[[nodiscard]] std::optional<RegexTemplate> DetectRegexTemplateFromBytes(std::string_view sniffBuffer);

/**
 * @brief Checks a pattern without parsing anything, so a dialog can report errors early.
 *
 * It returns true if the pattern is not empty, compiles, and has at least one named group written as (?<Name>...).
 * Otherwise errorOut gets a message worded like the one ParseStreaming gives. The compiled pattern is thrown away at
 * once, so this is a cheap check and not a setup for parsing.
 */
[[nodiscard]] bool ValidateRegexPattern(std::string_view pattern, std::string &errorOut);

/**
 * @brief Checks the pattern that recognizes the first line of a record. An empty anchor means the main pattern is
 * used. A non-empty one must compile but does not need named groups.
 */
[[nodiscard]] bool ValidateHeaderAnchor(std::string_view anchor, std::string &errorOut);

/**
 * @brief True if the pattern compiles and matches the whole line, which means a RegexParser would produce a row for it.
 *
 * It uses the same anchoring flags as detection. The regex template editor uses it to test a pattern against the sample
 * lines before saving. The compiled pattern is discarded, so it is for one-off checks and not for parsing. It returns
 * false for a pattern that does not compile, a line that does not match, and a match that goes over a PCRE2 limit.
 * Call ValidateRegexPattern first to get the compile error.
 */
[[nodiscard]] bool PatternMatchesLine(std::string_view pattern, std::string_view line);

} // namespace loglib
