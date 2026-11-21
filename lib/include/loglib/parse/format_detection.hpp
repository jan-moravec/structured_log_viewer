/**
 * @brief Decides which parser fits a file or a buffer of bytes.
 *
 * DetectFormat tries JSON, then logfmt, then the regex templates of a catalog, then CSV, and returns the first match as
 * a FormatSpec. For a regex match the spec holds the template's pattern and how it joins continuation lines. MakeParser
 * builds the parser for a FormatSpec. File, standard input and network detection all share these functions, so the same
 * bytes always give the same answer.
 *
 * This is part of the parse module.
 */
#pragma once

#include "loglib/config/format_spec.hpp"
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

/** @brief True if the sample starts with a line that looks like a CSV header and has a second line with as many cells.
 */
[[nodiscard]] bool LooksLikeCsv(std::string_view sample);

/**
 * @brief Detects the format of a sample.
 *
 * The formats are tried in this order and the first match wins: JSON, logfmt, the regex templates of the catalog, CSV.
 * For a regex match the result carries the pattern and continuation settings of the template. The result is empty for
 * an empty sample and for content nothing recognizes. A caller that can wait for more bytes, such as one reading a slow
 * network stream, can tell that it has not seen enough yet. A caller that always needs a format can write
 * value_or(FormatSpec{}), which gives JSON.
 *
 * Files, standard input and network streams all use this function, so the same bytes give the same result. A file is
 * detected from ReadProbeHead.
 */
[[nodiscard]] std::optional<FormatSpec> DetectFormat(
    std::string_view sample, const RegexTemplateCatalog &catalog = RegexTemplateCatalog::Builtin()
);

/**
 * @brief Says whether a sample fits a format that the caller has already chosen.
 *
 * JSON, logfmt and CSV use the LooksLike function of the format. Regex checks the pattern in spec, and not the
 * templates of a catalog, against the first non-blank lines of the sample with the same rule detection uses. An empty
 * pattern or one that does not compile does not fit. Use it to warn that a pinned format does not suit the input.
 */
[[nodiscard]] bool SampleFitsFormat(std::string_view sample, const FormatSpec &spec);

/**
 * @brief Creates the parser for a format and its settings.
 *
 * For the Regex format the parser uses the pattern, continuation mode and header anchor of the spec. An empty pattern
 * gives a parser that reports one error about the empty pattern through the sink. AutoDetectParser uses this to pick
 * its parser without depending on the application.
 */
[[nodiscard]] std::unique_ptr<LogParser> MakeParser(const FormatSpec &spec);

} // namespace loglib
