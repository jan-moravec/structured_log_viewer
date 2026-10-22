#include "slp_process.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
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
    std::ifstream input(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

void WriteFile(const std::filesystem::path &path, std::string_view bytes)
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
    return std::filesystem::path(SLP_EXECUTABLE);
}

std::filesystem::path FixturesDir()
{
    return std::filesystem::path(SLP_FIXTURES_DIR);
}

std::filesystem::path GoldenDir()
{
    return std::filesystem::path(SLP_GOLDEN_DIR);
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
            const unsigned char ch = static_cast<unsigned char>(text[i]);
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
                constexpr char hex[] = "0123456789abcdef";
                out.push_back(hex[ch >> 4]);
                out.push_back(hex[ch & 0x0f]);
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

#ifdef _WIN32
CommandResult RunSlp(const std::vector<std::string> &args)
{
    const std::filesystem::path outPath = std::filesystem::temp_directory_path() / "slp_e2e_out.txt";
    const std::filesystem::path errPath = std::filesystem::temp_directory_path() / "slp_e2e_err.txt";
    std::error_code ec;
    std::filesystem::remove(outPath, ec);
    std::filesystem::remove(errPath, ec);

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
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
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

    return CommandResult{.exitCode = static_cast<int>(code), .out = ReadFile(outPath), .err = ReadFile(errPath)};
}
#else
CommandResult RunSlp(const std::vector<std::string> &args)
{
    const std::filesystem::path outPath = std::filesystem::temp_directory_path() / "slp_e2e_out.txt";
    const std::filesystem::path errPath = std::filesystem::temp_directory_path() / "slp_e2e_err.txt";
    std::error_code ec;
    std::filesystem::remove(outPath, ec);
    std::filesystem::remove(errPath, ec);

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
        dup2(outFd, STDOUT_FILENO);
        dup2(errFd, STDERR_FILENO);
        close(outFd);
        close(errFd);
        execv(argv[0], argv.data());
        _exit(127);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    const int code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    return CommandResult{.exitCode = code, .out = ReadFile(outPath), .err = ReadFile(errPath)};
}
#endif

void RequireGolden(std::string_view relativeName, std::string_view actual)
{
    const std::filesystem::path path = GoldenDir() / std::string(relativeName);
    if (EnvIsSet("SLP_UPDATE_GOLDEN"))
    {
        WriteFile(path, actual);
        SUCCEED("updated golden " << path.string());
        return;
    }

    REQUIRE(std::filesystem::exists(path));
    const std::string expected = ReadFile(path);
    INFO(DescribeByteDiff(expected, actual));
    REQUIRE(actual == expected);
}

} // namespace slp::test
