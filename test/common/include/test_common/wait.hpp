/**
 * @brief Polling wait for tests, so a test waits for a condition and not for a fixed time.
 *
 * WaitUntil checks a predicate at a short interval until it holds or a deadline passes. The deadline only guards
 * against a hang, so it is generous and a passing test returns as soon as the condition holds. SleepToProveAbsence is
 * the one place a test waits a fixed time, for a check that something did not happen.
 *
 * This is shared by the library, CLI and app tests through test_common.
 */
#pragma once

#include <chrono>
#include <thread>

namespace test_common
{

/** @brief The deadline of WaitUntil when the caller gives none. It guards a hang and is never a speed claim. */
inline constexpr std::chrono::milliseconds DEFAULT_WAIT_DEADLINE{60000};

/** @brief The poll step of WaitUntil when the caller gives none. */
inline constexpr std::chrono::milliseconds DEFAULT_WAIT_INTERVAL{5};

/**
 * @brief Calls predicate until it returns true or deadline passes, pausing interval between calls.
 *
 * Returns whether the predicate held. It is checked once more at the deadline, so a condition that became true during
 * the last pause is not missed.
 */
template <class Predicate>
[[nodiscard]] bool WaitUntil(
    Predicate &&predicate,
    std::chrono::milliseconds deadline = DEFAULT_WAIT_DEADLINE,
    std::chrono::milliseconds interval = DEFAULT_WAIT_INTERVAL
)
{
    const auto giveUpAt = std::chrono::steady_clock::now() + deadline;
    while (!predicate())
    {
        if (std::chrono::steady_clock::now() >= giveUpAt)
        {
            return predicate();
        }
        std::this_thread::sleep_for(interval);
    }
    return true;
}

/**
 * @brief Waits a fixed time so a test can check that something did not happen.
 *
 * Polling cannot show an absence, so time is the only signal. Use it only where nothing observable marks the end of
 * the period, keep the time short, and say in the calling test what the quiet period proves.
 */
inline void SleepToProveAbsence(std::chrono::milliseconds quietPeriod)
{
    std::this_thread::sleep_for(quietPeriod);
}

} // namespace test_common
