#include "loglib/internal/stdin_peek.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <io.h>
#include <windows.h>
#else
#include <cerrno>
#include <poll.h>
#include <unistd.h>
#endif

namespace loglib::internal
{

bool IsStdinInteractive() noexcept
{
#ifdef _WIN32
    auto *const stdinHandle = ::GetStdHandle(STD_INPUT_HANDLE);
    if (stdinHandle == INVALID_HANDLE_VALUE || stdinHandle == nullptr)
    {
        return false;
    }
    // FILE_TYPE_CHAR covers both an interactive console and the
    // (rare) case of a printer / raw character device redirected
    // onto FD 0. Either would block the peek loop unboundedly;
    // treat both as "interactive".
    return ::GetFileType(stdinHandle) == FILE_TYPE_CHAR;
#else
    return ::isatty(STDIN_FILENO) != 0;
#endif
}

namespace
{

/// Wait for stdin to become readable, or until @p deadline
/// elapses. Returns true when at least one byte is available
/// (or EOF is observable, which `read`/`ReadFile` will report on
/// the next call). Returns false on timeout / hard error, in
/// which case the caller should stop peeking.
bool WaitForStdinReadable(std::chrono::steady_clock::time_point deadline)
{
#ifdef _WIN32
    auto *const stdinHandle = ::GetStdHandle(STD_INPUT_HANDLE);
    if (stdinHandle == INVALID_HANDLE_VALUE || stdinHandle == nullptr)
    {
        return false;
    }
    // `PeekNamedPipe` is the only non-blocking readiness probe
    // that works on anonymous pipes. It also correctly reports EOF
    // (returns 0 with ERROR_BROKEN_PIPE), in which case the
    // subsequent `ReadFile` returns 0 too -- treated as done.
    // For redirected regular files, reads never
    // block, so the peek-then-sleep loop degenerates into
    // pure reads.
    constexpr DWORD POLL_INTERVAL_MS = 20;
    while (true)
    {
        if (std::chrono::steady_clock::now() >= deadline)
        {
            return false;
        }
        DWORD available = 0;
        DWORD leftInMessage = 0;
        const BOOL ok = ::PeekNamedPipe(stdinHandle, nullptr, 0, nullptr, &available, &leftInMessage);
        if (ok == 0)
        {
            // Not a pipe (regular file / unknown): fall through
            // and let `ReadFile` handle it -- on a real file it
            // returns immediately, and on the interactive path
            // the caller was supposed to bail via
            // `IsStdinInteractive`.
            return true;
        }
        if (available > 0)
        {
            return true;
        }
        ::Sleep(POLL_INTERVAL_MS);
    }
#else
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline)
    {
        return false;
    }
    const auto remainingMs = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    ::pollfd pfd{};
    pfd.fd = STDIN_FILENO;
    pfd.events = POLLIN;
    const int rc = ::poll(&pfd, 1, static_cast<int>(std::min<std::int64_t>(remainingMs, INT32_MAX)));
    if (rc < 0)
    {
        // EINTR: caller loops with a fresh deadline slice; any
        // other error is terminal for the peek.
        return errno == EINTR;
    }
    if (rc == 0)
    {
        return false;
    }
    return (pfd.revents & (POLLIN | POLLHUP | POLLERR)) != 0;
#endif
}

} // namespace

std::string StdinPeek(std::size_t budget, std::chrono::milliseconds timeout)
{
    if (budget == 0)
    {
        return {};
    }

    std::string out;
    out.reserve(budget);

    const auto deadline = std::chrono::steady_clock::now() + timeout;

    constexpr std::size_t SCRATCH_BYTES = 4096;
    char scratch[SCRATCH_BYTES];

#ifdef _WIN32
    auto *const stdinHandle = ::GetStdHandle(STD_INPUT_HANDLE);
    if (stdinHandle == INVALID_HANDLE_VALUE || stdinHandle == nullptr)
    {
        return out;
    }
    while (out.size() < budget)
    {
        if (!WaitForStdinReadable(deadline))
        {
            break;
        }
        const DWORD want = static_cast<DWORD>(std::min<std::size_t>(SCRATCH_BYTES, budget - out.size()));
        DWORD got = 0;
        if (::ReadFile(stdinHandle, scratch, want, &got, nullptr) == 0)
        {
            // Broken pipe / handle closed / read error. Whatever
            // we have so far is what the peek will hand to the
            // detector; the producer will observe EOF too.
            break;
        }
        if (got == 0)
        {
            // Clean EOF.
            break;
        }
        out.append(scratch, got);
    }
#else
    while (out.size() < budget)
    {
        if (!WaitForStdinReadable(deadline))
        {
            break;
        }
        const std::size_t want = std::min<std::size_t>(SCRATCH_BYTES, budget - out.size());
        const ::ssize_t got = ::read(STDIN_FILENO, scratch, want);
        if (got > 0)
        {
            out.append(scratch, static_cast<std::size_t>(got));
            continue;
        }
        if (got == 0)
        {
            // Clean EOF.
            break;
        }
        if (errno == EINTR)
        {
            continue;
        }
        break;
    }
#endif

    return out;
}

} // namespace loglib::internal
