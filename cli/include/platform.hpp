#pragma once

#include <loglib/stop_token.hpp>
#include <loglib/time_zone_context.hpp>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace slp
{

/** @brief True when @p stream is a terminal (a console on Windows). */
[[nodiscard]] bool IsTerminal(std::FILE *stream) noexcept;

/**
 * @brief Puts stdout in binary mode and, when stdout is a console,
 * switches that console to UTF-8.
 *
 * The previous code page is restored by `RestoreConsoleOutput()`.
 */
void PrepareStandardStreams();

/** @brief Restores the console output code page saved by `PrepareStandardStreams()`. */
void RestoreConsoleOutput() noexcept;

/**
 * @brief Result of one cancel signal.
 *
 * The first signal requests stop. The next one terminates the process.
 */
enum class CancelStep
{
    Stop,
    Terminate,
};

/**
 * @brief Records one cancel signal on @p source.
 *
 * The first call requests stop and returns `Stop`. A later call
 * returns `Terminate`; the caller must exit immediately.
 */
[[nodiscard]] CancelStep AdvanceCancel(loglib::StopSource &source, std::atomic<int> &signals) noexcept;

/**
 * @brief Installs SIGINT, SIGTERM, and Windows CTRL_C_EVENT handlers.
 *
 * Handlers share @p source. A second signal restores the console
 * code page and terminates with exit code 130. On POSIX, SIGPIPE is
 * ignored so a closed pipe surfaces as `BrokenPipe` instead of
 * killing the process.
 */
void InstallSignalHandlers(loglib::StopSource &source);

/** @brief Absolute path of the running executable. */
[[nodiscard]] std::filesystem::path CurrentExecutable();

/**
 * @brief Locates a tzdata directory using the GUI search order.
 *
 * Checks @p executableDirectory`/tzdata`, then on macOS
 * `../Resources/tzdata`, then `$APPDIR/usr/share/tzdata` when
 * @p appDir is non-empty, then walks @p startDirectory toward the
 * root. @p searched receives every candidate, including misses.
 */
[[nodiscard]] std::filesystem::path FindTzdataDirectory(
    const std::filesystem::path &executableDirectory,
    const std::filesystem::path &startDirectory,
    const std::filesystem::path &appDir,
    std::vector<std::filesystem::path> &searched
);

/**
 * @brief Time zone selected for one invocation.
 *
 * `error` set means exit 2. `warning` is printed once unless the
 * user asked for `--quiet`. An empty tzdata path falls back to UTC.
 */
struct TimeZoneSetup
{
    loglib::TimeZoneContext context = loglib::TimeZoneContext::Utc();
    std::string warning;
    std::string error;
};

/**
 * @brief Resolves @p zoneName (`local`, `UTC`, or an IANA name).
 *
 * `--tz UTC` never warns. A missing tzdata directory warns and
 * falls back to UTC. An unknown name, when tzdata did load, is
 * reported in `error` with the library's reason.
 */
[[nodiscard]] TimeZoneSetup ResolveTimeZone(std::string_view zoneName, const std::filesystem::path &tzdataDirectory);

} // namespace slp
