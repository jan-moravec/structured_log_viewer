#pragma once

#include "commands.hpp"
#include "diagnostics.hpp"
#include "options.hpp"

#include <loglib/decompressing_byte_source.hpp>
#include <loglib/format_detection.hpp>
#include <loglib/log_parser.hpp>
#include <loglib/log_table.hpp>
#include <loglib/parser_options.hpp>
#include <loglib/stop_token.hpp>

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace slp
{

class Console;

/**
 * @brief Result of loading every positional source into one table.
 *
 * `ownedDecompressors` must outlive `table` while any row still
 * references decompressed backing storage. `format` is the first
 * source's resolved format. `linesConsumed` is the highest 1-based
 * physical line number observed across rows and parse errors.
 */
struct LoadedTable
{
    loglib::LogTable table{loglib::LogData{}, loglib::LogConfigurationManager{}};
    std::vector<std::string> parseErrors;
    std::vector<std::shared_ptr<loglib::DecompressingByteSource>> ownedDecompressors;
    std::optional<loglib::DetectedFormat> format;
    std::uint64_t linesConsumed = 0;
};

/** @brief Lower-case name of @p format, as `--format` spells it. */
[[nodiscard]] std::string_view FormatName(loglib::Source::Format format) noexcept;

/**
 * @brief Loads and parses `input.sources` into one table, in the order given.
 *
 * A bundle's embedded configuration seeds the table before the first
 * `BeginStreaming`. `input.config` seeds it the same way and replaces the
 * bundle's, minus its `source`. Warnings go to @p console as they occur.
 *
 * @param global Global flags.
 * @param input Sources and parser flags.
 * @param console Diagnostic output.
 * @param runtime Process context.
 * @return The loaded table, or the failure that stopped loading.
 */
[[nodiscard]] std::expected<LoadedTable, Failure> LoadTable(
    const GlobalOptions &global, const InputOptions &input, const Console &console, const RuntimeContext &runtime
);

/**
 * @brief Registers the user's regex templates for format detection.
 *
 * Reads `global.templateDir` or the library's user template directory.
 * Unreadable template files become warnings on @p console.
 */
void LoadUserTemplates(const GlobalOptions &global, const Console &console);

/**
 * @brief Builds the parser for a live byte stream.
 *
 * A pinned `--format` selects that parser; otherwise the parser detects
 * the format from the first bytes the producer delivers.
 *
 * @return The parser, or a usage failure when the regex pattern cannot be resolved.
 */
[[nodiscard]] std::expected<std::unique_ptr<loglib::LogParser>, Failure> MakeStreamParser(
    const GlobalOptions &global, const InputOptions &input
);

/**
 * @brief Parser options shared by every command that parses.
 *
 * @param global Global flags that supply `--threads`.
 * @param input Parser flags.
 * @param stop Token the parser polls between batches.
 */
[[nodiscard]] loglib::ParserOptions MakeParserOptions(
    const GlobalOptions &global, const InputOptions &input, const loglib::StopToken &stop
);

} // namespace slp
