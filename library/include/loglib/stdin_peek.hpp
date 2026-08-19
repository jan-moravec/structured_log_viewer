#pragma once

#include <chrono>
#include <cstddef>
#include <string>

namespace loglib
{

/**
 * @brief Returns whether standard input is an interactive terminal.
 *
 * Distinguishes a console from a pipe, redirected file, or closed
 * handle. Used by the stdin session opener to refuse the
 * bare-terminal case before `StdinPeek` can block the GUI thread.
 *
 * Windows treats every `FILE_TYPE_CHAR` handle as interactive;
 * POSIX uses `isatty(STDIN_FILENO)`. Both return false on a closed
 * or invalid stdin, which is the safer default (an invalid handle
 * triggers a soft error later, not a UI hang).
 *
 * @return `true` when stdin is an interactive terminal.
 */
[[nodiscard]] bool IsStdinInteractive() noexcept;

/**
 * @brief Reads up to @p budget bytes from standard input with a deadline.
 *
 * Returns the bytes read (possibly fewer than @p budget on slow
 * producers). Blocks on the calling thread; callers pass a soft
 * deadline so a chatty-but-slow producer does not freeze the GUI
 * while the peek gathers format evidence.
 *
 * The deadline is applied *between* syscalls: a `read` / `ReadFile`
 * already in the kernel is not cancelled, but the loop stops issuing
 * new syscalls once the deadline passes. The polling implementation
 * uses `poll` (POSIX) or `PeekNamedPipe` (Windows pipes) to avoid
 * parking in the kernel past the deadline; console handles are already
 * rejected upstream via `IsStdinInteractive`.
 *
 * Used by the stdin path (`--stdin` / `-`) as the first step of the
 * peek → detect → hand-off handshake: the peeked bytes are fed to
 * `DetectFormatFromBytes` and then re-injected into the streaming
 * parse loop via `ParserOptions::initialCarry`, so the stdin producer
 * only has to yield the remaining bytes.
 *
 * Runs on the GUI thread during app startup, before the producer's
 * worker thread exists, so no synchronisation is needed.
 *
 * @param budget Maximum bytes to read.
 * @param timeout Wall-clock budget for the peek.
 * @return Bytes consumed, possibly empty or shorter than @p budget.
 */
[[nodiscard]] std::string StdinPeek(std::size_t budget, std::chrono::milliseconds timeout);

} // namespace loglib
