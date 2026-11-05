/**
 * @brief Decides which parser fits a file or a buffer of bytes.
 *
 * Detection tries JSON, then logfmt, then the regex templates, then CSV, and returns the first match. For a regex match
 * it also returns the template's pattern and how it joins continuation lines. MakeParserForFormat builds the parser for
 * a chosen format from a FormatOptions, which pins the pattern and the continuation mode for regex and the continuation
 * switch for logfmt. File, standard input and network detection all share these functions, so the same bytes always
 * give the same answer.
 *
 * This is part of the parse module.
 */
#pragma once

#include "loglib/parse/log_parser.hpp"
#include "loglib/schema/log_configuration.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace loglib
{

/**
 * @brief The settings a parser needs besides its format.
 *
 * The regex fields apply to Format::Regex. The logfmt switch applies to Format::Logfmt. Other formats ignore them.
 */
struct FormatOptions
{
    /** @brief The PCRE2 pattern of the regex parser. An empty pattern makes the parser report one error. */
    std::string regexPattern;
    /** @brief How the regex parser joins continuation lines to the record before them. */
    ContinuationMode regexContinuationMode = ContinuationMode::None;
    /** @brief The pattern that starts a record when regexContinuationMode is UntilNextHeader. Empty uses regexPattern.
     */
    std::string regexHeaderAnchor;
    /** @brief Whether the logfmt parser joins lines that start with a space or tab to the record before them. */
    bool multilineLogfmt = true;
};

/**
 * @brief The detected format and, for a regex format, the pattern and continuation settings of the matching template.
 * The regex fields are empty for other formats and when nothing matched.
 */
struct DetectedFormat
{
    Source::Format format = Source::Format::Json;
    std::string regexPattern;
    ContinuationMode regexContinuationMode = ContinuationMode::None;
    std::string regexHeaderAnchor;

    /** @brief The FormatOptions that build the parser for this result. */
    [[nodiscard]] FormatOptions Options(bool multilineLogfmt = true) const
    {
        return FormatOptions{
            .regexPattern = regexPattern,
            .regexContinuationMode = regexContinuationMode,
            .regexHeaderAnchor = regexHeaderAnchor,
            .multilineLogfmt = multilineLogfmt,
        };
    }
};

/**
 * @brief Detects the format of a sample by returning the first format whose parser accepts it.
 *
 * When nothing matches, the result is JSON with an empty pattern, so callers that always need a parser can use it
 * directly. For that reason it cannot tell a real JSON match from a fallback. A caller that can wait for more bytes,
 * such as one reading a slow network stream, should use TryDetectFormatFromBytes instead.
 *
 * Files, standard input and network streams all use the same detection, so the same bytes give the same result.
 */
[[nodiscard]] DetectedFormat DetectFormatFromBytes(std::string_view sniffBuffer);

/**
 * @brief Like DetectFormatFromBytes, but returns nullopt when no parser accepts the sample.
 *
 * That is the case for an empty sample or content no parser recognizes. A caller reading a stream can tell that it
 * has not seen enough yet and read more, which the fallback to JSON in DetectFormatFromBytes would hide. The formats
 * are tried in the same order: JSON, logfmt, regex templates, CSV.
 */
[[nodiscard]] std::optional<DetectedFormat> TryDetectFormatFromBytes(std::string_view sniffBuffer);

/**
 * @brief Detects the format of a file from its first PROBE_BYTES_BUDGET bytes.
 *
 * A file that is missing or cannot be read gives JSON with an empty pattern, the same as an empty sample.
 */
[[nodiscard]] DetectedFormat DetectFormatForPath(const std::filesystem::path &file);

/**
 * @brief Creates a parser for a format.
 *
 * For Format::Regex the parser uses the pattern, continuation mode and header anchor of options. An empty pattern gives
 * a parser that reports one error about the empty pattern through the sink. AutoDetectParser uses this to pick its
 * parser without depending on the application.
 */
[[nodiscard]] std::unique_ptr<LogParser> MakeParserForFormat(Source::Format format, const FormatOptions &options = {});

} // namespace loglib
