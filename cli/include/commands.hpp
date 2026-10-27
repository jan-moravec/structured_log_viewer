#pragma once

#include "diagnostics.hpp"
#include "options.hpp"

#include <loglib/stop_token.hpp>
#include <loglib/time_zone_context.hpp>

#include <cstdio>

namespace slp
{

class Console;

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
 * @brief Runs `slp formats`. No log source is read.
 *
 * Unknown `--show` slugs exit 2 and name the closest slugs. Other
 * failures of a single user-template file are warnings and do not
 * change a successful exit code.
 */
[[nodiscard]] ExitCode RunFormats(const FormatsOptions &options, const GlobalOptions &global, Console &console);

/**
 * @brief Runs `slp query` over local file sources.
 *
 * Local paths expand through `ResolveSourcePlan` (unless
 * `--no-rotation-history`). Stdin and `.slvbundle` are supported.
 * Network sources remain `slp tail` only.
 */
[[nodiscard]] ExitCode RunQuery(
    const QueryOptions &options, const GlobalOptions &global, Console &console, const RuntimeContext &runtime
);

/**
 * @brief Runs `slp schema` over the same sources as `query`.
 *
 * Text and JSON renderings list columns, types, visibility, and
 * optional sample values. Exit 3 when the source yields no columns.
 */
[[nodiscard]] ExitCode RunSchema(
    const SchemaOptions &options, const GlobalOptions &global, Console &console, const RuntimeContext &runtime
);

/**
 * @brief Runs `slp validate` and reports parse / column-type health.
 *
 * Exit 0 when there are no parse errors, or when `--max-error-rate`
 * covers the observed rate. Otherwise exit 4.
 */
[[nodiscard]] ExitCode RunValidate(
    const ValidateOptions &options, const GlobalOptions &global, Console &console, const RuntimeContext &runtime
);

/**
 * @brief Runs `slp stats` over the same sources as `query`.
 *
 * Summarises by level, time bucket, or column value frequency.
 * Exit 1 when a `--filter` matches no rows; exit 0 on an empty source.
 */
[[nodiscard]] ExitCode RunStats(
    const StatsOptions &options, const GlobalOptions &global, Console &console, const RuntimeContext &runtime
);

/**
 * @brief Runs `slp bundle` and writes a `.slvbundle` of the loaded rows.
 *
 * By default every row is kept and the effective filter, sort, and
 * columns are stored in the embedded configuration. `--filtered-rows`
 * writes only matching rows and clears the embedded filter. Progress
 * goes to a rewritten stderr line when stderr is a terminal. Exit 130
 * when cancelled and exit 5 when the file cannot be written; neither
 * leaves a partial file.
 */
[[nodiscard]] ExitCode RunBundle(
    const BundleOptions &options, const GlobalOptions &global, Console &console, const RuntimeContext &runtime
);

/**
 * @brief Runs `slp tail` over one file, standard input, or a network listener.
 *
 * Rows go to stdout, or are appended to `--output`, one flushed write
 * per arriving batch. `--max-rows`, `--for`, end of standard input, and
 * a signal end the run with exit 0. An unopenable source or a bind
 * failure exits 3. Rows are emitted before retention evicts them.
 */
[[nodiscard]] ExitCode RunTail(
    const TailOptions &options, const GlobalOptions &global, Console &console, const RuntimeContext &runtime
);

/**
 * @brief Runs the parsed command with stdout and stderr as its streams.
 *
 * @param options Parsed invocation whose `kind` is `Run`.
 * @param runtime Process context.
 * @return The process exit code.
 */
[[nodiscard]] int Execute(const Options &options, const RuntimeContext &runtime);

} // namespace slp
