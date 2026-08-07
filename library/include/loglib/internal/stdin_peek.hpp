#pragma once

#include <cstddef>
#include <string>

namespace loglib::internal
{

/// Synchronously read up to @p budget bytes from the process's
/// standard input. Blocks until the budget is reached, stdin is
/// closed (EOF), or an I/O error occurs (returned as whatever was
/// read so far -- empty on hard failure). Returns the bytes read.
///
/// Used by the stdin path (`--stdin` / `-`) as the first step of
/// the "peek → detect → hand off" handshake: the peeked bytes are
/// fed to `DetectFormatFromBytes` and then re-injected into the
/// streaming parse loop via `ParserOptions::initialCarry`, so the
/// stdin producer only has to yield the *remaining* bytes.
///
/// This helper is a small platform shim over `read` (POSIX) /
/// `ReadFile` (Windows) on the raw stdin file descriptor / handle.
/// It runs on the GUI thread during app startup, before the
/// producer's worker thread exists, so no synchronisation is
/// needed.
[[nodiscard]] std::string StdinPeek(std::size_t budget);

} // namespace loglib::internal
