#include <loglib/format_detection.hpp>
#include <loglib/internal/stdin_peek.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_parser.hpp>

#include <loglib_test/scaled_ms.hpp>
#include <test_common/pipe.hpp>

#include <catch2/catch_all.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

using loglib::internal::IsStdinInteractive;
using loglib::internal::StdinPeek;
using loglib_test::ScaledMs;
using test_common::Pipe;
using namespace std::chrono_literals;

namespace
{

/// RAII helper: swap the process's standard input for @p pipe's
/// read end. Restores the previous stdin in the destructor so
/// sibling tests (and the CTest driver) see their original FD 0.
class StdinRedirect
{
public:
    explicit StdinRedirect(Pipe &pipe)
#ifdef _WIN32
        : mPrevious(::GetStdHandle(STD_INPUT_HANDLE))
#else
        : mPrevious(::dup(STDIN_FILENO))
#endif
    {
#ifdef _WIN32
        REQUIRE(::SetStdHandle(STD_INPUT_HANDLE, pipe.ReadEnd()) != 0);
#else
        REQUIRE(mPrevious >= 0);
        REQUIRE(::dup2(pipe.ReadEnd(), STDIN_FILENO) >= 0);
#endif
    }

    // NOLINTNEXTLINE(bugprone-exception-escape)
    ~StdinRedirect() noexcept
    {
#ifdef _WIN32
        ::SetStdHandle(STD_INPUT_HANDLE, mPrevious);
#else
        if (mPrevious >= 0)
        {
            ::dup2(mPrevious, STDIN_FILENO);
            ::close(mPrevious);
        }
#endif
    }

    StdinRedirect(const StdinRedirect &) = delete;
    StdinRedirect &operator=(const StdinRedirect &) = delete;

private:
#ifdef _WIN32
    HANDLE mPrevious = nullptr;
#else
    int mPrevious = -1;
#endif
};

} // namespace

TEST_CASE("IsStdinInteractive returns false when stdin is a pipe", "[StdinPeek]")
{
    // We can't portably assert the "true" branch without a real
    // controlling terminal, but the false branch is the one that
    // the app relies on to *not* refuse a piped stdin session.
    Pipe pipe;
    const StdinRedirect redirect(pipe);
    CHECK_FALSE(IsStdinInteractive());
}

TEST_CASE("StdinPeek delivers piped bytes up to the byte budget", "[StdinPeek]")
{
    Pipe pipe;
    const StdinRedirect redirect(pipe);

    const std::string payload = "hello, stdin peek\n";
    pipe.Write(payload);
    pipe.CloseWrite();

    const std::string peek = StdinPeek(/*budget=*/1024, ScaledMs(2000ms));
    CHECK(peek == payload);
}

TEST_CASE("StdinPeek honours the byte budget", "[StdinPeek]")
{
    Pipe pipe;
    const StdinRedirect redirect(pipe);

    // Producer writes more than the budget wants; only the first
    // `budget` bytes may be drained.
    std::string payload(4096, 'x');
    pipe.Write(payload);
    pipe.CloseWrite();

    const std::size_t budget = 128;
    const std::string peek = StdinPeek(budget, ScaledMs(2000ms));
    CHECK(peek.size() == budget);
    CHECK(peek == payload.substr(0, budget));
}

TEST_CASE("StdinPeek returns immediately on EOF", "[StdinPeek]")
{
    Pipe pipe;
    const StdinRedirect redirect(pipe);

    pipe.CloseWrite();

    const auto start = std::chrono::steady_clock::now();
    const std::string peek = StdinPeek(/*budget=*/1024, ScaledMs(5000ms));
    const auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(peek.empty());
    CHECK(elapsed < ScaledMs(1000ms));
}

TEST_CASE("StdinPeek stops at the timeout when the producer is silent", "[StdinPeek]")
{
    Pipe pipe;
    const StdinRedirect redirect(pipe);

    // Producer writes eventually, but the timeout is short enough
    // that the peek must give up first. Do NOT close the write
    // end: an EOF would let the peek exit early via the "clean
    // EOF" branch rather than the deadline branch.
    std::thread lateWriter([&] {
        std::this_thread::sleep_for(ScaledMs(2000ms));
        pipe.Write("late bytes\n");
    });

    const auto timeout = ScaledMs(200ms);
    const auto start = std::chrono::steady_clock::now();
    const std::string peek = StdinPeek(/*budget=*/1024, timeout);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(peek.empty());
    // Comfortable upper bound: 4x timeout tolerates syscall wake
    // latency + Windows PeekNamedPipe polling interval without
    // masking a true "peek never returned" regression.
    CHECK(elapsed < timeout * 4);

    // Let the late writer drain into the (still-attached but now
    // ignored) pipe so its join doesn't outlive the fixture.
    lateWriter.join();
    pipe.CloseWrite();
}

TEST_CASE(
    "StdinPeek bytes + DetectFormatFromBytes agree with DetectFormatForPath on the same content", "[StdinPeek]"
)
{
    // The stdin path drives its format decision through the
    // `peek -> DetectFormatFromBytes -> initialCarry` handshake.
    // File-backed opens go through `DetectFormatForPath`. Assert
    // both entry points reach the same verdict for identical
    // bytes so the two ingest routes are indistinguishable to a
    // saved configuration.
    struct Sample
    {
        std::string_view label;
        std::string_view bytes;
        loglib::LogConfiguration::Source::Format expected;
    };
    const Sample samples[] = {
        {"json",
         R"({"level":"info","message":"first"}
{"level":"warn","message":"second"}
)",
         loglib::LogConfiguration::Source::Format::Json},
        {"logfmt", "level=info msg=first\nlevel=warn msg=second\n", loglib::LogConfiguration::Source::Format::Logfmt},
        {"csv", "level,message\ninfo,first\nwarn,second\n", loglib::LogConfiguration::Source::Format::Csv},
        {"regex-syslog",
         "Apr 28 04:02:03 host-a systemd: System starting\n"
         "Jun 27 01:47:20 host-b configd[17]: network changed\n",
         loglib::LogConfiguration::Source::Format::Regex},
    };

    for (const Sample &s : samples)
    {
        INFO("sample: " << s.label);

        // Stdin path.
        Pipe pipe;
        const StdinRedirect redirect(pipe);
        pipe.Write(s.bytes);
        pipe.CloseWrite();
        const std::string peek = StdinPeek(loglib::PROBE_BYTES_BUDGET, ScaledMs(2000ms));
        REQUIRE_FALSE(peek.empty());
        const loglib::DetectedFormat viaStdin = loglib::DetectFormatFromBytes(peek);

        // File path.
        const auto tempPath = std::filesystem::temp_directory_path() /
                              (std::string("stdin_peek_parity_") + std::string(s.label) + ".log");
        {
            std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
            REQUIRE(out.is_open());
            out.write(s.bytes.data(), static_cast<std::streamsize>(s.bytes.size()));
        }
        const loglib::DetectedFormat viaFile = loglib::DetectFormatForPath(tempPath);
        std::error_code ec;
        std::filesystem::remove(tempPath, ec);

        CHECK(viaStdin.format == s.expected);
        CHECK(viaFile.format == s.expected);
        CHECK(viaStdin.format == viaFile.format);
        CHECK(viaStdin.regexPattern == viaFile.regexPattern);
    }
}
