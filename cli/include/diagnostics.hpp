#pragma once

#include "options.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace slp
{

/** @brief Process status values documented in `--help` and `doc/cli.md`. */
enum class ExitCode : int
{
    Success = 0,
    Empty = 1,
    Usage = 2,
    Input = 3,
    Data = 4,
    Output = 5,
    Cancelled = 130,
};

/**
 * @brief Exit-code table printed at the end of every `--help` text.
 *
 * Each line states the code and the condition that produces it.
 */
[[nodiscard]] std::string_view ExitCodeHelpEpilog() noexcept;

/** @brief One stderr line, `slp: ...` or `slp <command>: ...`. */
[[nodiscard]] std::string FormatErrorLine(std::string_view command, std::string_view sentence);

/** @brief Warning line. Callers omit it entirely when `--quiet` is set. */
[[nodiscard]] std::string FormatWarningLine(std::string_view sentence);

/**
 * @brief Verbose stage line `stage=... rows=... ms=...`.
 *
 * `milliseconds` is whole milliseconds. The line has no `slp:` prefix
 * so it stays easy to grep.
 */
[[nodiscard]] std::string FormatStageLine(std::string_view stage, std::uint64_t rows, std::uint64_t milliseconds);

/** @brief Writes a usage error: the `slp:` line, then the usage line. */
void WriteUsage(const UsageError &error);

/** @brief Writes an error line to stderr. `--quiet` does not suppress it. */
void WriteError(std::string_view command, std::string_view sentence);

/**
 * @brief Writes a warning line to stderr unless @p quiet is set.
 *
 * Warnings never change the process exit code.
 */
void WriteWarning(bool quiet, std::string_view sentence);

/**
 * @brief Writes a stage line when @p verboseLevel is greater than zero.
 */
void WriteStage(int verboseLevel, std::string_view stage, std::uint64_t rows, std::uint64_t milliseconds);

/**
 * @brief Formats a malformed `--filter` diagnostic with a caret line.
 *
 * The result is ready for stderr: the `slp:` header, the query,
 * a caret under @p offset, and the parser message.
 */
[[nodiscard]] std::string FormatFilterParseDiagnostic(
    std::string_view query, std::size_t offset, std::string_view message
);

} // namespace slp

