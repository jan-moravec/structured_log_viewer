/**
 * @brief Retries a file operation while Windows reports the file as busy.
 *
 * Virus scanners, indexers and sync clients briefly open files that were just written, which makes a rename fail with
 * an access or sharing error. Callers wrap the operation in RetryWhileFileBusy and only the busy errors are retried,
 * for up to one second. Used wherever the library replaces a file on disk.
 *
 * This is part of the core module.
 */
#pragma once

#include <chrono>
#include <filesystem>
#include <system_error>
#include <thread>

namespace loglib::internal
{

/**
 * @brief Runs a file operation again while Windows reports the file as busy.
 *
 * Virus scanners, search indexers and sync clients briefly open files that were just written, which makes a rename fail
 * with an access or sharing error. Those two errors are retried for up to one second. Any other result returns at once,
 * and so does every result on other platforms. The attempt is a callable that does the operation and returns its
 * std::error_code. The result is the one from the last attempt.
 */
template <typename Attempt> [[nodiscard]] std::error_code RetryWhileFileBusy(const Attempt &attempt)
{
#ifdef _WIN32
    constexpr int ERROR_ACCESS_DENIED_CODE = 5;
    constexpr int ERROR_SHARING_VIOLATION_CODE = 32;
    constexpr std::chrono::seconds RETRY_TIMEOUT{1};
    constexpr std::chrono::milliseconds RETRY_INTERVAL{20};

    const auto deadline = std::chrono::steady_clock::now() + RETRY_TIMEOUT;
    while (true)
    {
        const std::error_code error = attempt();
        const bool busy = error.category() == std::system_category() &&
                          (error.value() == ERROR_ACCESS_DENIED_CODE || error.value() == ERROR_SHARING_VIOLATION_CODE);
        if (!busy || std::chrono::steady_clock::now() >= deadline)
        {
            return error;
        }
        std::this_thread::sleep_for(RETRY_INTERVAL);
    }
#else
    return attempt();
#endif
}

/**
 * @brief Renames a file, retrying while it is busy as RetryWhileFileBusy does. An existing file at the new path is
 * replaced.
 */
[[nodiscard]] inline std::error_code RenameRetryingWhileFileBusy(
    const std::filesystem::path &from, const std::filesystem::path &to
)
{
    return RetryWhileFileBusy([&from, &to] {
        std::error_code error;
        std::filesystem::rename(from, to, error);
        return error;
    });
}

} // namespace loglib::internal
