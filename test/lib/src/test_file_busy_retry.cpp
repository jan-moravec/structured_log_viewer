#include <loglib/core/internal/file_busy_retry.hpp>

#include <test_common/temp_dir.hpp>

#include <catch2/catch_all.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

using loglib::internal::RenameRetryingWhileFileBusy;
using loglib::internal::RetryWhileFileBusy;
using namespace std::chrono_literals;

namespace
{

constexpr int ERROR_ACCESS_DENIED_CODE = 5;
constexpr int ERROR_SHARING_VIOLATION_CODE = 32;

std::error_code BusyError(int code = ERROR_SHARING_VIOLATION_CODE)
{
    return {code, std::system_category()};
}

} // namespace

TEST_CASE("RetryWhileFileBusy returns a success without a second attempt", "[file_busy_retry]")
{
    int attempts = 0;
    const std::error_code result = RetryWhileFileBusy(
        [&] {
            ++attempts;
            return std::error_code{};
        },
        50ms,
        1ms
    );
    CHECK_FALSE(result);
    CHECK(attempts == 1);
}

TEST_CASE("RetryWhileFileBusy returns an error that is not a busy error at once", "[file_busy_retry]")
{
    int attempts = 0;
    const std::error_code result = RetryWhileFileBusy(
        [&] {
            ++attempts;
            return std::make_error_code(std::errc::no_such_file_or_directory);
        },
        5s,
        1ms
    );
    CHECK(result == std::errc::no_such_file_or_directory);
    CHECK(attempts == 1);
}

#ifdef _WIN32
TEST_CASE("RetryWhileFileBusy tries again while the file is busy and returns the first success", "[file_busy_retry]")
{
    for (const int code : {ERROR_ACCESS_DENIED_CODE, ERROR_SHARING_VIOLATION_CODE})
    {
        int attempts = 0;
        const std::error_code result = RetryWhileFileBusy(
            [&] {
                ++attempts;
                return attempts < 3 ? BusyError(code) : std::error_code{};
            },
            60s,
            1ms
        );
        CHECK_FALSE(result);
        CHECK(attempts == 3);
    }
}

TEST_CASE("RetryWhileFileBusy gives up after the timeout and returns the last busy error", "[file_busy_retry]")
{
    int attempts = 0;
    const auto start = std::chrono::steady_clock::now();
    const std::error_code result = RetryWhileFileBusy(
        [&] {
            ++attempts;
            return BusyError();
        },
        50ms,
        5ms
    );
    const auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(result == BusyError());
    CHECK(attempts > 1);
    CHECK(elapsed >= 50ms);
}

TEST_CASE("RetryWhileFileBusy with a zero timeout makes one attempt", "[file_busy_retry]")
{
    int attempts = 0;
    const std::error_code result = RetryWhileFileBusy(
        [&] {
            ++attempts;
            return BusyError(ERROR_ACCESS_DENIED_CODE);
        },
        0ms,
        1ms
    );
    CHECK(result == BusyError(ERROR_ACCESS_DENIED_CODE));
    CHECK(attempts == 1);
}
#else
TEST_CASE("RetryWhileFileBusy does not retry on this platform", "[file_busy_retry]")
{
    int attempts = 0;
    const std::error_code result = RetryWhileFileBusy(
        [&] {
            ++attempts;
            return BusyError();
        },
        5s,
        1ms
    );
    CHECK(result == BusyError());
    CHECK(attempts == 1);
}
#endif

TEST_CASE("RenameRetryingWhileFileBusy replaces an existing file", "[file_busy_retry]")
{
    const test_common::TempDir directory("rename_retry");
    const std::filesystem::path from = directory.Write("from.txt", "new");
    const std::filesystem::path to = directory.Write("to.txt", "old");

    CHECK_FALSE(RenameRetryingWhileFileBusy(from, to));

    std::ifstream input(to, std::ios::binary);
    const std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    CHECK(contents == "new");
    CHECK_FALSE(std::filesystem::exists(from));
}
