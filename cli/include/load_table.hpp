#pragma once

#include "commands.hpp"
#include "options.hpp"

#include <loglib/decompressing_byte_source.hpp>
#include <loglib/log_table.hpp>

#include <cstdint>
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
[[nodiscard]] std::pair<LoadedTable, CommandOutput> LoadTable(
    const Options &options, const RuntimeContext &runtime
);

} // namespace slp
