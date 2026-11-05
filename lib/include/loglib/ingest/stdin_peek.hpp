/**
 * @brief Looks at the start of standard input before committing to a format.
 *
 * IsStdinInteractive tells a terminal apart from a pipe or file, so callers can refuse a bare terminal. StdinPeek reads
 * a limited number of bytes within a time limit, which format detection then inspects.
 *
 * This is part of the ingest module.
 */
#pragma once

#include <chrono>
#include <cstddef>
#include <string>

namespace loglib
{

/**
 * @brief Returns true if standard input is an interactive terminal.
 *
 * A pipe, a redirected file and a closed handle are not interactive. Call it before StdinPeek, which would otherwise
 * block waiting for someone to type. On Windows every character device counts as interactive, and on POSIX the check is
 * isatty. A closed or invalid input gives false, so the problem shows up as an error on the first read and not as a
 * hang.
 */
[[nodiscard]] bool IsStdinInteractive() noexcept;

/**
 * @brief Reads up to budget bytes from standard input, giving up after the timeout.
 *
 * It returns what it read, which can be fewer bytes than the budget when the writer is slow, or none. The caller's
 * thread blocks. The timeout is checked between reads: a read already waiting in the operating system is not cancelled,
 * but no new one is started once time is up. Reads are guarded by poll on POSIX and PeekNamedPipe for Windows pipes so
 * that they do not wait past the deadline.
 *
 * It is the first step of reading standard input. The bytes go to format detection and are then given back to the
 * parser through StreamLineSource::UnreadBytes, so the stdin producer delivers only what comes after. It runs before
 * the producer's worker thread exists, so it needs no locking.
 */
[[nodiscard]] std::string StdinPeek(std::size_t budget, std::chrono::milliseconds timeout);

} // namespace loglib
