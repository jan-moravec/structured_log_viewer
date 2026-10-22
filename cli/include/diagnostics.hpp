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
    Internal = 70,
    Cancelled = 130,
};

/**
 * @brief A command step that ended the run.
 *
 * `text` is the complete stderr report, one or more newline-terminated
 * lines, or empty when the exit code says enough (cancellation).
 */
struct Failure
{
    ExitCode code = ExitCode::Internal;
    std::string text;
};

/**
 * @brief Builds a one-line failure, `slp <command>: <sentence>.`
 *
 * @param code Exit code of the failure.
 * @param command Subcommand name, or empty for the bare `slp:` prefix.
 * @param sentence Reason without the prefix.
 */
[[nodiscard]] Failure MakeFailure(ExitCode code, std::string_view command, std::string_view sentence);

/** @brief Failure for a cancelled run; it prints nothing and exits 130. */
[[nodiscard]] Failure CancelledFailure();

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
 * @brief Formats a malformed `--filter` diagnostic with a caret line.
 *
 * The result is ready for stderr: the `slp:` header, the query,
 * a caret under @p offset, and the parser message.
 */
[[nodiscard]] std::string FormatFilterParseDiagnostic(
    std::string_view query, std::size_t offset, std::string_view message
);

} // namespace slp
