#include "slp_process.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string_view>
#include <system_error>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#endif

#ifndef SLP_EXECUTABLE
#error "SLP_EXECUTABLE must be defined to the built slp binary"
#endif

#ifndef SLP_FIXTURES_DIR
#error "SLP_FIXTURES_DIR must be defined"
#endif

#ifndef SLP_GOLDEN_DIR
#error "SLP_GOLDEN_DIR must be defined"
#endif

namespace slp::test
{

namespace
{

[[nodiscard]] std::string ReadFile(const std::filesystem::path &path)
{
    const std::ifstream input(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

void WritePathBytes(const std::filesystem::path &path, std::string_view bytes)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    REQUIRE(static_cast<bool>(output));
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(static_cast<bool>(output));
}

[[nodiscard]] bool EnvIsSet(const char *name)
{
#ifdef _WIN32
    char buffer[8];
    const DWORD n = GetEnvironmentVariableA(name, buffer, static_cast<DWORD>(sizeof(buffer)));
    return n > 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND;
#else
    return std::getenv(name) != nullptr;
#endif
}

} // namespace

std::filesystem::path SlpBinary()
{
    return {SLP_EXECUTABLE};
}

std::filesystem::path FixturesDir()
{
    return {SLP_FIXTURES_DIR};
}

std::filesystem::path GoldenDir()
{
    return {SLP_GOLDEN_DIR};
}

std::string DescribeByteDiff(std::string_view expected, std::string_view actual)
{
    std::size_t offset = 0;
    const std::size_t limit = std::min(expected.size(), actual.size());
    while (offset < limit && expected[offset] == actual[offset])
    {
        ++offset;
    }
    if (offset == expected.size() && offset == actual.size())
    {
        return "byte streams are identical";
    }

    std::ostringstream message;
    message << "first difference at offset " << offset << " (expected size " << expected.size() << ", actual size "
            << actual.size() << ")";
    const auto snippet = [](std::string_view text, std::size_t at) {
        const std::size_t begin = at > 16 ? at - 16 : 0;
        const std::size_t end = std::min(text.size(), at + 16);
        std::string out;
        for (std::size_t i = begin; i < end; ++i)
        {
            const auto ch = static_cast<unsigned char>(text[i]);
            if (ch == '\n')
            {
                out += "\\n";
            }
            else if (ch == '\r')
            {
                out += "\\r";
            }
            else if (ch < 0x20 || ch >= 0x7f)
            {
                out += '\\';
                out += 'x';
                constexpr char HEX_DIGITS[] = "0123456789abcdef";
                out.push_back(HEX_DIGITS[ch >> 4]);
                out.push_back(HEX_DIGITS[ch & 0x0f]);
            }
            else
            {
                out.push_back(static_cast<char>(ch));
            }
        }
        return out;
    };
    message << "\n  expected: ..." << snippet(expected, offset) << "...";
    message << "\n  actual:   ..." << snippet(actual, offset) << "...";
    return message.str();
}

namespace
{

#ifdef _WIN32
[[nodiscard]] std::filesystem::path UniqueTempPath(const wchar_t *prefix)
{
    wchar_t directory[MAX_PATH];
    REQUIRE(GetTempPathW(MAX_PATH, directory) != 0);
    wchar_t file[MAX_PATH];
    REQUIRE(GetTempFileNameW(directory, prefix, 0, file) != 0);
    return {file};
}

[[nodiscard]] CommandResult RunSlpImpl(const std::vector<std::string> &args, HANDLE stdinHandle)
{
    const std::filesystem::path outPath = UniqueTempPath(L"slpo");
    const std::filesystem::path errPath = UniqueTempPath(L"slpe");

    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    HANDLE outFile = CreateFileW(
        outPath.wstring().c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        &security,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    HANDLE errFile = CreateFileW(
        errPath.wstring().c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        &security,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    REQUIRE(outFile != INVALID_HANDLE_VALUE);
    REQUIRE(errFile != INVALID_HANDLE_VALUE);

    std::wstring command = L"\"" + SlpBinary().wstring() + L"\"";
    for (const std::string &arg : args)
    {
        command.push_back(L' ');
        command.push_back(L'"');
        command.append(arg.begin(), arg.end());
        command.push_back(L'"');
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = stdinHandle;
    startup.hStdOutput = outFile;
    startup.hStdError = errFile;

    PROCESS_INFORMATION process{};
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    const BOOL created =
        CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &process);
    CloseHandle(outFile);
    CloseHandle(errFile);
    REQUIRE(created);

    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);

    CommandResult result{.exitCode = static_cast<int>(code), .out = ReadFile(outPath), .err = ReadFile(errPath)};
    std::error_code ec;
    std::filesystem::remove(outPath, ec);
    std::filesystem::remove(errPath, ec);
    return result;
}
#else
[[nodiscard]] std::filesystem::path UniqueTempPath(const char *prefix)
{
    const std::filesystem::path pattern = std::filesystem::temp_directory_path() / (std::string(prefix) + "XXXXXX");
    std::string mutablePattern = pattern.string();
    const int fd = ::mkstemp(mutablePattern.data());
    REQUIRE(fd >= 0);
    ::close(fd);
    return std::filesystem::path(mutablePattern);
}

[[nodiscard]] CommandResult RunSlpImpl(const std::vector<std::string> &args, int stdinFd)
{
    const std::filesystem::path outPath = UniqueTempPath("slpo");
    const std::filesystem::path errPath = UniqueTempPath("slpe");

    std::vector<std::string> storage;
    storage.reserve(args.size() + 1);
    storage.push_back(SlpBinary().string());
    storage.insert(storage.end(), args.begin(), args.end());
    std::vector<char *> argv;
    argv.reserve(storage.size() + 1);
    for (std::string &value : storage)
    {
        argv.push_back(value.data());
    }
    argv.push_back(nullptr);

    const pid_t pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0)
    {
        const int outFd = ::open(outPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        const int errFd = ::open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (outFd < 0 || errFd < 0)
        {
            _exit(127);
        }
        if (stdinFd >= 0)
        {
            dup2(stdinFd, STDIN_FILENO);
            close(stdinFd);
        }
        dup2(outFd, STDOUT_FILENO);
        dup2(errFd, STDERR_FILENO);
        close(outFd);
        close(errFd);
        execv(argv[0], argv.data());
        _exit(127);
    }

    if (stdinFd >= 0)
    {
        close(stdinFd);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    const int code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    CommandResult result{.exitCode = code, .out = ReadFile(outPath), .err = ReadFile(errPath)};
    std::error_code ec;
    std::filesystem::remove(outPath, ec);
    std::filesystem::remove(errPath, ec);
    return result;
}
#endif

} // namespace

#ifdef _WIN32
CommandResult RunSlp(const std::vector<std::string> &args)
{
    return RunSlpImpl(args, GetStdHandle(STD_INPUT_HANDLE));
}

CommandResult RunSlpWithStdin(const std::vector<std::string> &args, std::string_view stdinBytes)
{
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    REQUIRE(CreatePipe(&readPipe, &writePipe, &security, 0));
    REQUIRE(SetHandleInformation(writePipe, HANDLE_FLAG_INHERIT, 0));

    // Launch first so a large payload cannot fill the pipe before the
    // child has started consuming. Write after CreateProcess returns.
    const std::filesystem::path outPath = UniqueTempPath(L"slpo");
    const std::filesystem::path errPath = UniqueTempPath(L"slpe");

    SECURITY_ATTRIBUTES fileSecurity{};
    fileSecurity.nLength = sizeof(fileSecurity);
    fileSecurity.bInheritHandle = TRUE;

    HANDLE outFile = CreateFileW(
        outPath.wstring().c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        &fileSecurity,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    HANDLE errFile = CreateFileW(
        errPath.wstring().c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        &fileSecurity,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    REQUIRE(outFile != INVALID_HANDLE_VALUE);
    REQUIRE(errFile != INVALID_HANDLE_VALUE);

    std::wstring command = L"\"" + SlpBinary().wstring() + L"\"";
    for (const std::string &arg : args)
    {
        command.push_back(L' ');
        command.push_back(L'"');
        command.append(arg.begin(), arg.end());
        command.push_back(L'"');
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = readPipe;
    startup.hStdOutput = outFile;
    startup.hStdError = errFile;

    PROCESS_INFORMATION process{};
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    const BOOL created =
        CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &process);
    CloseHandle(outFile);
    CloseHandle(errFile);
    CloseHandle(readPipe);
    REQUIRE(created);

    std::size_t offset = 0;
    while (offset < stdinBytes.size())
    {
        DWORD written = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(stdinBytes.size() - offset, 1 << 16));
        REQUIRE(::WriteFile(writePipe, stdinBytes.data() + offset, chunk, &written, nullptr));
        offset += written;
    }
    CloseHandle(writePipe);

    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);

    CommandResult result{.exitCode = static_cast<int>(code), .out = ReadFile(outPath), .err = ReadFile(errPath)};
    std::error_code ec;
    std::filesystem::remove(outPath, ec);
    std::filesystem::remove(errPath, ec);
    return result;
}
#else
CommandResult RunSlp(const std::vector<std::string> &args)
{
    return RunSlpImpl(args, -1);
}

CommandResult RunSlpWithStdin(const std::vector<std::string> &args, std::string_view stdinBytes)
{
    int pipefds[2] = {-1, -1};
    REQUIRE(::pipe(pipefds) == 0);

    const std::filesystem::path outPath = UniqueTempPath("slpo");
    const std::filesystem::path errPath = UniqueTempPath("slpe");

    std::vector<std::string> storage;
    storage.reserve(args.size() + 1);
    storage.push_back(SlpBinary().string());
    storage.insert(storage.end(), args.begin(), args.end());
    std::vector<char *> argv;
    argv.reserve(storage.size() + 1);
    for (std::string &value : storage)
    {
        argv.push_back(value.data());
    }
    argv.push_back(nullptr);

    const pid_t pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0)
    {
        close(pipefds[1]);
        const int outFd = ::open(outPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        const int errFd = ::open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (outFd < 0 || errFd < 0)
        {
            _exit(127);
        }
        dup2(pipefds[0], STDIN_FILENO);
        close(pipefds[0]);
        dup2(outFd, STDOUT_FILENO);
        dup2(errFd, STDERR_FILENO);
        close(outFd);
        close(errFd);
        execv(argv[0], argv.data());
        _exit(127);
    }

    close(pipefds[0]);
    std::size_t offset = 0;
    while (offset < stdinBytes.size())
    {
        const ssize_t written = ::write(pipefds[1], stdinBytes.data() + offset, stdinBytes.size() - offset);
        REQUIRE(written > 0);
        offset += static_cast<std::size_t>(written);
    }
    close(pipefds[1]);

    int status = 0;
    waitpid(pid, &status, 0);
    const int code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    CommandResult result{.exitCode = code, .out = ReadFile(outPath), .err = ReadFile(errPath)};
    std::error_code ec;
    std::filesystem::remove(outPath, ec);
    std::filesystem::remove(errPath, ec);
    return result;
}
#endif

struct SlpChild::Impl
{
    std::filesystem::path outPath;
    std::filesystem::path errPath;
#ifdef _WIN32
    PROCESS_INFORMATION process{};
    bool exited = false;
    int exitCode = -1;
#else
    pid_t pid = -1;
    bool exited = false;
    int exitCode = -1;
#endif
};

#ifdef _WIN32
SlpChild::SlpChild(const std::vector<std::string> &args)
    : mImpl(std::make_unique<Impl>())
{
    mImpl->outPath = UniqueTempPath(L"slpo");
    mImpl->errPath = UniqueTempPath(L"slpe");

    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    const auto open = [&](const std::filesystem::path &path, DWORD access, DWORD share, DWORD disposition) {
        return CreateFileW(
            path.wstring().c_str(), access, share, &security, disposition, FILE_ATTRIBUTE_NORMAL, nullptr
        );
    };
    HANDLE nul = open(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_EXISTING);
    HANDLE outFile = open(mImpl->outPath, GENERIC_WRITE, FILE_SHARE_READ, CREATE_ALWAYS);
    HANDLE errFile = open(mImpl->errPath, GENERIC_WRITE, FILE_SHARE_READ, CREATE_ALWAYS);
    REQUIRE(nul != INVALID_HANDLE_VALUE);
    REQUIRE(outFile != INVALID_HANDLE_VALUE);
    REQUIRE(errFile != INVALID_HANDLE_VALUE);

    std::wstring command = L"\"" + SlpBinary().wstring() + L"\"";
    for (const std::string &arg : args)
    {
        command.push_back(L' ');
        command.push_back(L'"');
        command.append(arg.begin(), arg.end());
        command.push_back(L'"');
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = nul;
    startup.hStdOutput = outFile;
    startup.hStdError = errFile;

    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    const BOOL created = CreateProcessW(
        nullptr, mutableCommand.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &mImpl->process
    );
    CloseHandle(nul);
    CloseHandle(outFile);
    CloseHandle(errFile);
    REQUIRE(created);
}

SlpChild::~SlpChild()
{
    if (!mImpl->exited)
    {
        TerminateProcess(mImpl->process.hProcess, 1);
        WaitForSingleObject(mImpl->process.hProcess, INFINITE);
    }
    CloseHandle(mImpl->process.hThread);
    CloseHandle(mImpl->process.hProcess);
    std::error_code ec;
    std::filesystem::remove(mImpl->outPath, ec);
    std::filesystem::remove(mImpl->errPath, ec);
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static): the POSIX variant signals the child.
bool SlpChild::Interrupt()
{
    // A console Ctrl+C cannot be aimed at one child without a new process group.
    return false;
}

std::optional<int> SlpChild::Wait(std::chrono::milliseconds timeout)
{
    if (!mImpl->exited)
    {
        if (WaitForSingleObject(mImpl->process.hProcess, static_cast<DWORD>(timeout.count())) != WAIT_OBJECT_0)
        {
            return std::nullopt;
        }
        DWORD code = 1;
        GetExitCodeProcess(mImpl->process.hProcess, &code);
        mImpl->exitCode = static_cast<int>(code);
        mImpl->exited = true;
    }
    return mImpl->exitCode;
}
#else
SlpChild::SlpChild(const std::vector<std::string> &args)
    : mImpl(std::make_unique<Impl>())
{
    mImpl->outPath = UniqueTempPath("slpo");
    mImpl->errPath = UniqueTempPath("slpe");

    std::vector<std::string> storage;
    storage.reserve(args.size() + 1);
    storage.push_back(SlpBinary().string());
    storage.insert(storage.end(), args.begin(), args.end());
    std::vector<char *> argv;
    argv.reserve(storage.size() + 1);
    for (std::string &value : storage)
    {
        argv.push_back(value.data());
    }
    argv.push_back(nullptr);

    mImpl->pid = fork();
    REQUIRE(mImpl->pid >= 0);
    if (mImpl->pid == 0)
    {
        const int inFd = ::open("/dev/null", O_RDONLY);
        const int outFd = ::open(mImpl->outPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        const int errFd = ::open(mImpl->errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (inFd < 0 || outFd < 0 || errFd < 0)
        {
            _exit(127);
        }
        dup2(inFd, STDIN_FILENO);
        dup2(outFd, STDOUT_FILENO);
        dup2(errFd, STDERR_FILENO);
        close(inFd);
        close(outFd);
        close(errFd);
        execv(argv[0], argv.data());
        _exit(127);
    }
}

SlpChild::~SlpChild()
{
    if (!mImpl->exited)
    {
        ::kill(mImpl->pid, SIGKILL);
        int status = 0;
        waitpid(mImpl->pid, &status, 0);
    }
    std::error_code ec;
    std::filesystem::remove(mImpl->outPath, ec);
    std::filesystem::remove(mImpl->errPath, ec);
}

bool SlpChild::Interrupt()
{
    return ::kill(mImpl->pid, SIGINT) == 0;
}

std::optional<int> SlpChild::Wait(std::chrono::milliseconds timeout)
{
    if (mImpl->exited)
    {
        return mImpl->exitCode;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;)
    {
        int status = 0;
        const pid_t done = waitpid(mImpl->pid, &status, WNOHANG);
        if (done == mImpl->pid)
        {
            mImpl->exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
            mImpl->exited = true;
            return mImpl->exitCode;
        }
        if (std::chrono::steady_clock::now() >= deadline)
        {
            return std::nullopt;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
#endif

std::string SlpChild::Out() const
{
    return ReadFile(mImpl->outPath);
}

std::string SlpChild::Err() const
{
    return ReadFile(mImpl->errPath);
}

bool SlpChild::WaitForErr(std::string_view needle, std::chrono::milliseconds timeout) const
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (Err().contains(needle))
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return Err().contains(needle);
}

bool SlpChild::WaitForOut(std::string_view needle, std::chrono::milliseconds timeout) const
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (Out().contains(needle))
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return Out().contains(needle);
}

void RequireGolden(std::string_view relativeName, std::string_view actual)
{
    const std::filesystem::path path = GoldenDir() / std::string(relativeName);
    if (EnvIsSet("SLP_UPDATE_GOLDEN"))
    {
        WritePathBytes(path, actual);
        SUCCEED("updated golden " << path.string());
        return;
    }

    REQUIRE(std::filesystem::exists(path));
    const std::string expected = ReadFile(path);
    INFO(DescribeByteDiff(expected, actual));
    REQUIRE(actual == expected);
}

} // namespace slp::test
