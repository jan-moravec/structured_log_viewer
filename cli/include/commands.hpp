#pragma once

#include "options.hpp"

#include <loglib/stop_token.hpp>
#include <loglib/time_zone_context.hpp>

#include <string>

namespace slp
{

/** @brief Captured stdout, stderr, and exit code of one command. */
struct CommandOutput
{
    int exitCode = 0;
    std::string out;
    std::string err;
};

/**
 * @brief Process-wide state shared by every command.
 *
 * `stop` is the signal-handler token. `timeZone` is the resolved
 * `--tz` context; callers also install it as the process default.
 */
struct RuntimeContext
{
    loglib::StopToken stop;
    loglib::TimeZoneContext timeZone = loglib::TimeZoneContext::Utc();
};

/**
 * @brief Renders `slp formats` without touching the process streams.
 *
 * Unknown `--show` slugs exit 2 and name the closest slugs. Other
 * failures of a single user-template file are warnings on `err`
 * and do not change a successful exit code.
 */
[[nodiscard]] CommandOutput RenderFormats(const Options &options);

/**
 * @brief Runs `slp query` over local file sources.
 *
 * Local paths expand through `ResolveSourcePlan` (unless
 * `--no-rotation-history`). Stdin and `.slvbundle` are supported.
 * Network sources remain `slp tail` only.
 */
[[nodiscard]] CommandOutput RunQuery(const Options &options, const RuntimeContext &runtime);

/**
 * @brief Runs `slp schema` over the same sources as `query`.
 *
 * Text and JSON renderings list columns, types, visibility, and
 * optional sample values. Exit 3 when the source yields no columns.
 */
[[nodiscard]] CommandOutput RunSchema(const Options &options, const RuntimeContext &runtime);

/**
 * @brief Runs `slp validate` and reports parse / column-type health.
 *
 * Exit 0 when there are no parse errors, or when `--max-error-rate`
 * covers the observed rate. Otherwise exit 4.
 */
[[nodiscard]] CommandOutput RunValidate(const Options &options, const RuntimeContext &runtime);

/**
 * @brief Runs `slp stats` over the same sources as `query`.
 *
 * Summarises by level, time bucket, or column value frequency.
 * Exit 1 when a `--filter` matches no rows; exit 0 on an empty source.
 */
[[nodiscard]] CommandOutput RunStats(const Options &options, const RuntimeContext &runtime);

/**
 * @brief Runs the parsed command.
 *
 * `formats`, `query`, `schema`, `validate`, and `stats` are implemented.
 * Every other command reports that it is not available yet and
 * returns exit code 2.
 */
[[nodiscard]] int Execute(const Options &options, const RuntimeContext &runtime);

} // namespace slp
