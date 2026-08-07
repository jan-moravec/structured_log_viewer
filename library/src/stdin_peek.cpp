#include "loglib/internal/stdin_peek.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#include <cerrno>
#endif

namespace loglib::internal
{

std::string StdinPeek(std::size_t budget)
{
    if (budget == 0)
    {
        return {};
    }

    std::string out;
    out.reserve(budget);

#ifdef _WIN32
    // `GetStdHandle(STD_INPUT_HANDLE)` returns a HANDLE for the
    // process's stdin. It is safe to `ReadFile` on it directly;
    // the returned handle is *not* to be closed here (it is a
    // shared process-wide standard handle).
    const HANDLE stdinHandle = ::GetStdHandle(STD_INPUT_HANDLE);
    if (stdinHandle == INVALID_HANDLE_VALUE || stdinHandle == nullptr)
    {
        return out;
    }
    // Fixed-size scratch buffer so we don't have to grow `out`'s
    // internal buffer while `ReadFile` writes into it. 4 KiB is
    // one page; the OS routes pipe / console reads through the
    // same syscall regardless.
    constexpr DWORD SCRATCH_BYTES = 4096;
    char scratch[SCRATCH_BYTES];
    while (out.size() < budget)
    {
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
    constexpr std::size_t SCRATCH_BYTES = 4096;
    char scratch[SCRATCH_BYTES];
    while (out.size() < budget)
    {
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
        // got == -1: retry on EINTR, bail on anything else.
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
