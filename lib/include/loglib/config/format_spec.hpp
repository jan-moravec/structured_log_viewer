/**
 * @brief A log format and its settings, as one value.
 *
 * A FormatSpec says which parser reads a log and how it is set up: the pattern and continuation rules of the regex
 * format, or the line joining of logfmt. Format detection returns one, the parser factory builds a parser from one, and
 * a saved source stores one, so the same information is never kept in three shapes.
 *
 * This is part of the config module, because the configuration file saves it.
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace loglib
{

/**
 * @brief The parser a source is read with.
 *
 * In the saved file it is a string such as json, logfmt, csv or regex. New values go at the end and the order never
 * changes.
 */
enum class LogFormat
{
    Json,
    Logfmt,
    Csv,
    /** @brief The regex parser. It needs a pattern and refuses to run without one. */
    Regex,
};

/**
 * @brief How a regex template recognizes continuation lines.
 *
 * - None: every line is parsed on its own.
 * - Indented: lines that start with a space or tab continue the previous record.
 * - UntilNextHeader: lines that do not match the header pattern continue the previous record.
 *
 * With any mode but None, the continuation text is appended to the last named capture of the pattern, which must be a
 * string.
 */
enum class ContinuationMode : uint8_t
{
    None = 0,
    Indented,
    UntilNextHeader,
};

/** @brief The name used for a mode in saved files. */
[[nodiscard]] std::string_view ContinuationModeName(ContinuationMode mode) noexcept;

/** @brief Reads a mode from its saved name. Returns nullopt for an unknown name. */
[[nodiscard]] std::optional<ContinuationMode> ParseContinuationMode(std::string_view name) noexcept;

/** @brief The settings of the regex format. */
struct RegexSettings
{
    /** @brief The PCRE2 pattern, with named groups written as (?<Name>...). An empty pattern makes the parser report
     * one error. */
    std::string pattern;

    /** @brief How lines after the first line of a record are joined to it. */
    ContinuationMode continuation = ContinuationMode::None;

    /** @brief The pattern that recognizes the first line of a record. Empty means the pattern itself does. */
    std::string headerAnchor;

    friend bool operator==(const RegexSettings &, const RegexSettings &) = default;
};

/** @brief A format together with the settings that apply to it. */
struct FormatSpec
{
    LogFormat format = LogFormat::Json;

    /** @brief Used by the Regex format only. It is empty for the other formats. */
    RegexSettings regex;

    /** @brief Whether the logfmt parser joins lines that start with a space or tab to the record before them. */
    bool logfmtMultiline = true;

    friend bool operator==(const FormatSpec &, const FormatSpec &) = default;
};

} // namespace loglib
