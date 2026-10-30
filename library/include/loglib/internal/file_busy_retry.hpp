#pragma once

#include <chrono>
#include <system_error>
#include <thread>

namespace loglib::internal
{

/**
 * @brief Runs a file operation, retrying while Windows reports the file as busy.
 *
 * File scanners, indexers, and sync clients briefly open files that were
 * just written, which makes a rename fail with an access or sharing error.
 * Those errors are retried for up to one second. Every other result, and
 * every result on other platforms, returns at once.
 *
 * @param attempt Callable that performs the operation and returns its `std::error_code`.
 * @return The result of the last attempt.
 */
template <typename Attempt> [[nodiscard]] std::error_code RetryWhileFileBusy(Attempt &&attempt)
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

} // namespace loglib::internal
