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

#include <string>
#include <string_view>

namespace loglib
{

class FileLineSource;
class StreamLineSource;

/**
 * @brief Parses lines with a PCRE2 pattern that has named groups. The group names become the column keys.
 *
 * The pattern is not read from the file, as a CSV header is. It comes from the constructor, together with the way
 * continuation lines are joined. Calling Parse with an empty pattern reports one parse error.
 *
 * The compiled pattern is shared by all worker threads, and each thread has its own match data. Match and depth limits
 * contain patterns that backtrack too much. A line that goes over a limit becomes a parse error and parsing goes on.
 *
 * PCRE2 with JIT was chosen over RE2 because it was faster on patterns with many captures and it runs the patterns of
 * lnav and logstash-grok unchanged. CONTRIBUTING.md has the evaluation.
 *
 * Limits:
 * - A pattern matches one physical line. With a continuation mode other than None, the parser joins more lines into
 *   one record. The default mode is None, which keeps every line on its own.
 * - The pattern is configuration and not part of the file, so detection only finds files that match a template in a
 *   RegexTemplateCatalog. A file that needs a custom pattern that is not in the catalog can be opened as a network stream
 *   with a custom pattern, or by restoring a session whose source is already set to the regex format with that pattern.
 */
class RegexParser : public LogParser
{
public:
    /**
     * @brief Creates a parser for a pattern.
     *
     * An empty pattern is accepted and makes Parse report one error.
     *
     * @param pattern The PCRE2 pattern, with named groups written as (?<Name>...).
     * @param continuationMode How lines after the first line of a record are recognized.
     * @param headerAnchor The pattern that recognizes the first line of a record when continuationMode is
     * UntilNextHeader. If empty, the main pattern is used. It must compile, or Parse reports one error.
     */
    explicit RegexParser(
        std::string pattern, ContinuationMode continuationMode = ContinuationMode::None, std::string headerAnchor = {}
    );

    /** @brief Parses a static file with the pattern given to the constructor. */
    void Parse(FileLineSource &source, LogParseSink &sink, ParserOptions options = {}) const override;

    /** @brief Parses a live stream with the pattern given to the constructor. */
    void Parse(StreamLineSource &source, LogParseSink &sink, ParserOptions options = {}) const override;

private:
    std::string mPattern;
    ContinuationMode mContinuationMode;
    std::string mHeaderAnchor;
};

/**
 * @brief Checks a pattern without parsing anything, so a dialog can report errors early.
 *
 * It returns true if the pattern is not empty, compiles, and has at least one named group written as (?<Name>...).
 * Otherwise errorOut gets a message worded like the one Parse gives. The compiled pattern is thrown away at
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
