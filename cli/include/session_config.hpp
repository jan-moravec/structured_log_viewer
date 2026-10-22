#pragma once

#include "commands.hpp"
#include "load_table.hpp"
#include "options.hpp"

#include <loglib/log_configuration.hpp>
#include <loglib/log_table.hpp>
#include <loglib/row_ordering.hpp>

#include <cstddef>
#include <expected>
#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

namespace slp
{

/** @brief Sort column and direction after precedence resolution. */
struct SortChoice
{
    std::size_t columnIndex = 0;
    loglib::SortDirection direction = loglib::SortDirection::Ascending;
};

/**
 * @brief Session state after bundle, `--config`, and flags are merged.
 *
 * `expression` and `sort` drive row selection. `visibleColumns` lists
 * table column indices in display order. `configuration` is the
 * effective post-flag configuration written by the save flags; its
 * `source` is empty until `SaveSessionFiles` fills it.
 */
struct EffectiveSession
{
    loglib::FilterExpression expression;
    std::optional<SortChoice> sort;
    std::vector<std::size_t> visibleColumns;
    loglib::LogConfiguration configuration;
};

/**
 * @brief Reads a session or columns file for `--config`.
 *
 * The embedded `source` is discarded so the file never rebinds the
 * sources named on the command line.
 *
 * @param path Configuration file.
 * @return The configuration, or a one-sentence reason.
 */
[[nodiscard]] std::expected<loglib::LogConfiguration, std::string> LoadConfigFile(const std::filesystem::path &path);

/**
 * @brief Resolves the effective session for @p table.
 *
 * Precedence, lowest to highest:
 *  1. Bundle-embedded configuration, seeded by `LoadTable`.
 *  2. `--config`, which `LoadTable` seeds in place of the bundle's.
 *  3. Individual flags: `--filter`, `--since`, `--until`, `--min-level`,
 *     `--invert`, `--sort`, `--columns`, and `--all-columns`.
 *
 * This step starts from the table's seeded configuration. Any row
 * flag replaces the configuration's expression; `--invert` alone negates
 * it. `--sort` replaces the configuration's sort. `--all-columns` or
 * `--columns` replaces column visibility, and `--columns` also moves the
 * listed columns first in the written configuration.
 *
 * @param options Parsed invocation.
 * @param table Loaded table whose configuration holds layers 1 and 2.
 * @param runtime Process context for time zone resolution.
 * @param command Subcommand name for error lines.
 * @return The merged session, or a failing `CommandOutput`.
 */
[[nodiscard]] std::expected<EffectiveSession, CommandOutput> ApplySessionConfiguration(
    const Options &options, const loglib::LogTable &table, const RuntimeContext &runtime, std::string_view command
);

/**
 * @brief Writes `--save-session` and `--save-columns` when requested.
 *
 * The session file gets `SaveScope::Full` with `source` set to the
 * user-facing locators from `options.sources`: the primary path, not
 * rotated siblings, and the compressed file, not its decompressed
 * copy. Standard input is omitted. When no locator remains the
 * `source` is empty. The columns file gets `SaveScope::ColumnsOnly`.
 *
 * @param options Parsed invocation.
 * @param loaded Load result that supplies the detected format.
 * @param session Effective session to persist.
 * @return `std::nullopt` on success, or a failing `CommandOutput`
 * (exit 5) naming the file that could not be written.
 */
[[nodiscard]] std::optional<CommandOutput> SaveSessionFiles(
    const Options &options, const LoadedTable &loaded, const EffectiveSession &session
);

} // namespace slp
