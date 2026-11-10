/**
 * @brief Retries a file operation while Windows reports the file as busy.
 *
 * Virus scanners, indexers and sync clients briefly open files that were just written, which makes a rename fail with
 * an access or sharing error. Callers wrap the operation in RetryWhileFileBusy and only the busy errors are retried,
 * for up to one second by default. Used wherever the library replaces a file on disk.
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

/** @brief How long RetryWhileFileBusy keeps trying when the caller gives no timeout. */
inline constexpr std::chrono::milliseconds DEFAULT_BUSY_RETRY_TIMEOUT{1000};

/** @brief How long RetryWhileFileBusy pauses between attempts when the caller gives no interval. */
inline constexpr std::chrono::milliseconds DEFAULT_BUSY_RETRY_INTERVAL{20};

/**
 * @brief Runs a file operation again while Windows reports the file as busy.
 *
 * Virus scanners, search indexers and sync clients briefly open files that were just written, which makes a rename fail
 * with an access or sharing error. Those two errors are retried until the timeout has passed, one second unless the
 * caller gives another, with a pause of interval between attempts. Any other result returns at once, and so does every
 * result on other platforms. The attempt is a callable that does the operation and returns its std::error_code. The
 * result is the one from the last attempt. The attempt always runs at least once.
 */
template <typename Attempt>
[[nodiscard]] std::error_code RetryWhileFileBusy(
    const Attempt &attempt,
    [[maybe_unused]] std::chrono::milliseconds timeout = DEFAULT_BUSY_RETRY_TIMEOUT,
    [[maybe_unused]] std::chrono::milliseconds interval = DEFAULT_BUSY_RETRY_INTERVAL
)
{
#ifdef _WIN32
    constexpr int ERROR_ACCESS_DENIED_CODE = 5;
    constexpr int ERROR_SHARING_VIOLATION_CODE = 32;

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true)
    {
        const std::error_code error = attempt();
        const bool busy = error.category() == std::system_category() &&
                          (error.value() == ERROR_ACCESS_DENIED_CODE || error.value() == ERROR_SHARING_VIOLATION_CODE);
        if (!busy || std::chrono::steady_clock::now() >= deadline)
        {
            return error;
        }
        std::this_thread::sleep_for(interval);
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
    const std::filesystem::path &from,
    const std::filesystem::path &to,
    std::chrono::milliseconds timeout = DEFAULT_BUSY_RETRY_TIMEOUT
)
{
    return RetryWhileFileBusy(
        [&from, &to] {
            std::error_code error;
            std::filesystem::rename(from, to, error);
            return error;
        },
        timeout
    );
}

} // namespace loglib::internal
