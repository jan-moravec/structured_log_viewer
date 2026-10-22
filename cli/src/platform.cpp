#include "platform.hpp"

#include "diagnostics.hpp"

#include <cstdint>
#include <cstdlib>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <signal.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace slp
{

namespace
{

constexpr unsigned int WINDOWS_PATH_CHARS = 32768;
constexpr std::string_view UNAVAILABLE_ZONE = "is unavailable";

#ifdef _WIN32
UINT gPreviousOutputCp = 0;
bool gRestoreOutputCp = false;
#endif

std::atomic<int> *gSignalCount = nullptr;
loglib::StopSource *gStopSource = nullptr;

void OnCancelSignal()
{
    if (gSignalCount == nullptr || gStopSource == nullptr)
    {
        RestoreConsoleOutput();
        std::_Exit(static_cast<int>(ExitCode::Cancelled));
    }
    if (AdvanceCancel(*gStopSource, *gSignalCount) == CancelStep::Terminate)
    {
        RestoreConsoleOutput();
        std::_Exit(static_cast<int>(ExitCode::Cancelled));
    }
}

#ifdef _WIN32
BOOL WINAPI ConsoleCtrlHandler(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT)
    {
        OnCancelSignal();
        return TRUE;
    }
    return FALSE;
}
#else
void PosixCancelHandler(int /*signal*/)
{
    OnCancelSignal();
}
#endif

bool DirectoryExists(const std::filesystem::path &candidate)
{
    std::error_code error;
    const bool exists = std::filesystem::is_directory(candidate, error);
    return exists && !error;
}

} // namespace

bool IsTerminal(std::FILE *stream) noexcept
{
    if (stream == nullptr)
    {
        return false;
    }
#ifdef _WIN32
    return _isatty(_fileno(stream)) != 0;
#else
    return ::isatty(fileno(stream)) != 0;
#endif
}

int TerminalWidth(std::FILE *stream, int fallback) noexcept
{
    if (!IsTerminal(stream))
    {
        return fallback;
    }
#ifdef _WIN32
    const HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(handle, &info) != 0)
    {
        const int width = static_cast<int>(info.srWindow.Right - info.srWindow.Left + 1);
        return width > 0 ? width : fallback;
    }
    return fallback;
#else
    if (const char *columns = std::getenv("COLUMNS"))
    {
        char *end = nullptr;
        const long parsed = std::strtol(columns, &end, 10);
        if (end != columns && parsed > 0)
        {
            return static_cast<int>(parsed);
        }
    }
    struct winsize size {};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0)
    {
        return static_cast<int>(size.ws_col);
    }
    return fallback;
#endif
}

void PrepareStandardStreams()
{
#ifdef _WIN32
    (void)_setmode(_fileno(stdout), _O_BINARY);
    if (IsTerminal(stdout))
    {
        gPreviousOutputCp = GetConsoleOutputCP();
        if (SetConsoleOutputCP(CP_UTF8) != 0)
        {
            gRestoreOutputCp = true;
        }
    }
#else
    // POSIX stdout is already a byte stream.
#endif
}

void RestoreConsoleOutput() noexcept
{
#ifdef _WIN32
    if (gRestoreOutputCp)
    {
        SetConsoleOutputCP(gPreviousOutputCp);
        gRestoreOutputCp = false;
    }
#endif
}

CancelStep AdvanceCancel(loglib::StopSource &source, std::atomic<int> &signals) noexcept
{
    const int seen = signals.fetch_add(1, std::memory_order_acq_rel);
    source.request_stop();
    return seen == 0 ? CancelStep::Stop : CancelStep::Terminate;
}

void InstallSignalHandlers(loglib::StopSource &source)
{
    static std::atomic<int> signals{0};
    gSignalCount = &signals;
    gStopSource = &source;

#ifdef _WIN32
    (void)SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
#else
    struct sigaction action{};
    action.sa_handler = PosixCancelHandler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    (void)sigaction(SIGINT, &action, nullptr);
    (void)sigaction(SIGTERM, &action, nullptr);

    struct sigaction ignorePipe{};
    ignorePipe.sa_handler = SIG_IGN;
    sigemptyset(&ignorePipe.sa_mask);
    ignorePipe.sa_flags = 0;
    (void)sigaction(SIGPIPE, &ignorePipe, nullptr);
#endif
}

std::filesystem::path CurrentExecutable()
{
#ifdef _WIN32
    std::wstring buffer(WINDOWS_PATH_CHARS, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    buffer.resize(length);
    return std::filesystem::path(buffer);
#elif defined(__APPLE__)
    constexpr std::uint32_t INITIAL_PATH_CHARS = 1024;
    std::string buffer(INITIAL_PATH_CHARS, '\0');
    std::uint32_t size = INITIAL_PATH_CHARS;
    if (_NSGetExecutablePath(buffer.data(), &size) != 0)
    {
        buffer.assign(size, '\0');
        if (_NSGetExecutablePath(buffer.data(), &size) != 0)
        {
            return {};
        }
    }
    return std::filesystem::weakly_canonical(buffer.c_str());
#else
    std::error_code error;
    auto path = std::filesystem::read_symlink("/proc/self/exe", error);
    if (error)
    {
        return {};
    }
    return path;
#endif
}

std::filesystem::path FindTzdataDirectory(
    const std::filesystem::path &executableDirectory,
    const std::filesystem::path &startDirectory,
    const std::filesystem::path &appDir,
    std::vector<std::filesystem::path> &searched
)
{
    auto consider = [&searched](std::filesystem::path candidate) -> bool {
        const bool found = DirectoryExists(candidate);
        searched.push_back(std::move(candidate));
        return found;
    };

    if (!executableDirectory.empty() && consider(executableDirectory / "tzdata"))
    {
        return searched.back();
    }

#ifdef __APPLE__
    if (!executableDirectory.empty() && consider(executableDirectory.parent_path() / "Resources" / "tzdata"))
    {
        return searched.back();
    }
#endif

#if !defined(_WIN32) && !defined(__APPLE__)
    if (!appDir.empty() && consider(appDir / "usr" / "share" / "tzdata"))
    {
        return searched.back();
    }
#else
    (void)appDir;
#endif

    std::filesystem::path walk = startDirectory;
    while (!walk.empty())
    {
        if (consider(walk / "tzdata"))
        {
            return searched.back();
        }
        const std::filesystem::path parent = walk.parent_path();
        if (parent.empty() || parent == walk)
        {
            break;
        }
        walk = parent;
    }
    return {};
}

TimeZoneSetup ResolveTimeZone(std::string_view zoneName, const std::filesystem::path &tzdataDirectory)
{
    TimeZoneSetup setup;
    if (zoneName == "UTC")
    {
        return setup;
    }

    if (tzdataDirectory.empty())
    {
        setup.warning = "timezone data was not found; times use UTC.";
        return setup;
    }

    try
    {
        if (zoneName == "local")
        {
            setup.context = loglib::TimeZoneContext::Load(tzdataDirectory);
        }
        else
        {
            setup.context = loglib::TimeZoneContext::Load(tzdataDirectory, zoneName);
        }
    }
    catch (const std::exception &ex)
    {
        const std::string_view reason = ex.what();
        if (reason.find(UNAVAILABLE_ZONE) != std::string_view::npos)
        {
            setup.error = std::string(reason);
            return setup;
        }
        setup.warning = "timezone data at '" + tzdataDirectory.string() + "' could not be loaded; times use UTC.";
        setup.context = loglib::TimeZoneContext::Utc();
    }
    return setup;
}

} // namespace slp
