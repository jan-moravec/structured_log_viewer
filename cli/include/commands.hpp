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
 * Stdin, network, and `.slvbundle` sources are rejected until the
 * later source-resolution work lands. Rotation expansion is not
 * applied here yet.
 */
[[nodiscard]] CommandOutput RunQuery(const Options &options, const RuntimeContext &runtime);

/**
 * @brief Runs the parsed command.
 *
 * `formats` and `query` are implemented. Every other command
 * reports that it is not available yet and returns exit code 2.
 */
[[nodiscard]] int Execute(const Options &options, const RuntimeContext &runtime);

} // namespace slp
