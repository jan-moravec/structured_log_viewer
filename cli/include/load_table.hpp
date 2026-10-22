#pragma once

#include "commands.hpp"
#include "options.hpp"

#include <loglib/decompressing_byte_source.hpp>
#include <loglib/log_parse_sink.hpp>
#include <loglib/log_parser.hpp>
#include <loglib/log_table.hpp>
#include <loglib/parser_options.hpp>

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace slp
{

/**
 * @brief Result of loading every positional source into one table.
 *
 * `ownedDecompressors` must outlive `table` while any row still
 * references decompressed backing storage. `detectedFormat` is the
 * first source's resolved format name. `linesConsumed` is the highest
 * 1-based physical line number observed across rows and parse errors.
 */
struct LoadedTable
{
    loglib::LogTable table{loglib::LogData{}, loglib::LogConfigurationManager{}};
    std::vector<std::string> parseErrors;
    std::vector<std::shared_ptr<loglib::DecompressingByteSource>> ownedDecompressors;
    std::string detectedFormat;
    /** @brief Regex pattern of the first source when it resolved to `regex`. */
    std::string detectedRegexPattern;
    std::uint64_t linesConsumed = 0;
};

/**
 * @brief Loads and parses `options.sources` the same way `slp query` does.
 *
 * On failure the paired `CommandOutput` carries a non-zero exit code
 * and stderr text; the table may be incomplete. Warnings (verbose
 * source listing, template-load notes) live on that output's `err`
 * even when the exit code is zero.
 *
 * A bundle's embedded configuration seeds the table before the first
 * `BeginStreaming`. `--config` seeds it the same way and replaces the
 * bundle's, minus its `source`.
 */
[[nodiscard]] std::pair<LoadedTable, CommandOutput> LoadTable(const Options &options, const RuntimeContext &runtime);

/**
 * @brief Registers the user's regex templates for format detection.
 *
 * Reads `options.templateDir` or the library's user template directory.
 * Unreadable template files become warning lines on @p out.
 */
void LoadUserTemplates(const Options &options, CommandOutput &out);

/**
 * @brief Splices @p batch into @p table as `LoadTable` does.
 *
 * A newly appended Time column moves to index 0 and pending Level
 * promotions are applied, so column order matches the GUI.
 */
void AppendStreamedBatch(loglib::LogTable &table, loglib::StreamedBatch batch);

/**
 * @brief Builds the parser for a live byte stream.
 *
 * A pinned `--format` selects that parser; otherwise the parser detects
 * the format from the first bytes the producer delivers.
 *
 * @return The parser, or a failing `CommandOutput` (exit 2) when the
 * regex pattern cannot be resolved.
 */
[[nodiscard]] std::expected<std::unique_ptr<loglib::LogParser>, CommandOutput> MakeStreamParser(const Options &options);

/**
 * @brief Parser options for a live stream governed by @p stop.
 *
 * @param options Parsed invocation.
 * @param stop Token the parser polls between batches.
 */
[[nodiscard]] loglib::ParserOptions StreamParserOptions(const Options &options, const loglib::StopToken &stop);

} // namespace slp
