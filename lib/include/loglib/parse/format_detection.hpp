/**
 * @brief Decides which parser fits a file or a buffer of bytes.
 *
 * DetectFormat tries JSON, then logfmt, then the regex templates of a catalog, then CSV, and returns the first match. For
 * a regex match it also returns the template's pattern and how it joins continuation lines. MakeParserForFormat builds the parser for
 * a chosen format from a FormatOptions, which pins the pattern and the continuation mode for regex and the continuation
 * switch for logfmt. File, standard input and network detection all share these functions, so the same bytes always
 * give the same answer.
 *
 * This is part of the parse module.
 */
#pragma once

#include "loglib/config/log_configuration.hpp"
#include "loglib/parse/log_parser.hpp"
#include "loglib/parse/regex_templates.hpp"

#include <cstddef>
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
 * @brief How many bytes of a file or stream format detection looks at. Every kind of input uses the same budget, so the
 * same bytes give the same result.
 */
inline constexpr std::size_t PROBE_BYTES_BUDGET = 16 * 1024;

/**
 * @brief Reads up to budget bytes from the start of a file. A file that is missing or unreadable gives an empty
 * string.
 */
[[nodiscard]] std::string ReadProbeHead(const std::filesystem::path &file, std::size_t budget = PROBE_BYTES_BUDGET);

/** @brief True if the first non-blank line of the sample is a JSON object. */
[[nodiscard]] bool LooksLikeJson(std::string_view sample);

/** @brief True if the first non-blank line of the sample looks like key=value pairs. */
[[nodiscard]] bool LooksLikeLogfmt(std::string_view sample);

/** @brief True if the sample starts with a line that looks like a CSV header and has a second line with as many cells. */
[[nodiscard]] bool LooksLikeCsv(std::string_view sample);

/**
 * @brief Detects the format of a sample.
 *
 * The formats are tried in this order and the first match wins: JSON, logfmt, the regex templates of the catalog, CSV.
 * For a regex match the result carries the pattern and continuation settings of the template. The result is empty for an
 * empty sample and for content nothing recognizes. A caller that can wait for more bytes, such as one reading a slow
 * network stream, can tell that it has not seen enough yet. A caller that always needs a format can write
 * value_or(DetectedFormat{}), which gives JSON.
 *
 * Files, standard input and network streams all use this function, so the same bytes give the same result. A file is
 * detected from ReadProbeHead.
 */
[[nodiscard]] std::optional<DetectedFormat> DetectFormat(
    std::string_view sample, const RegexTemplateCatalog &catalog = RegexTemplateCatalog::Builtin()
);

/**
 * @brief Says whether a sample fits a format that the caller has already chosen.
 *
 * JSON, logfmt and CSV use the LooksLike function of the format. Regex checks the pattern in options, and not the
 * templates of a catalog, against the first non-blank lines of the sample with the same rule detection uses. An empty
 * pattern or one that does not compile does not fit. Use it to warn that a pinned format does not suit the input.
 */
[[nodiscard]] bool SampleFitsFormat(std::string_view sample, Source::Format format, const FormatOptions &options = {});

/**
 * @brief Creates a parser for a format.
 *
 * For Format::Regex the parser uses the pattern, continuation mode and header anchor of options. An empty pattern gives
 * a parser that reports one error about the empty pattern through the sink. AutoDetectParser uses this to pick its
 * parser without depending on the application.
 */
[[nodiscard]] std::unique_ptr<LogParser> MakeParserForFormat(Source::Format format, const FormatOptions &options = {});

} // namespace loglib
