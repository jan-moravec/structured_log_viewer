/**
 * @brief Polling wait for tests, so a test waits for a condition and not for a fixed time.
 *
 * WaitUntil checks a predicate at a short interval until it holds or a deadline passes. The deadline only guards
 * against a hang, so it is generous and a passing test returns as soon as the condition holds. SleepToProveAbsence is
 * the one place a test waits a fixed time, for a check that something did not happen. WaitForPasses waits on the pass
 * count of a producer with a worker thread, and LetWaiterPark gives a thread that is about to block a moment to do so.
 *
 * This is shared by the library, CLI and app tests through test_common.
 */
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
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
 * The predicate is taken by value, so pass a lambda that captures by reference. Returns whether the predicate held. It
 * is checked once more at the deadline, so a condition that became true during the last pause is not missed.
 */
template <class Predicate>
[[nodiscard]] bool WaitUntil(
    Predicate predicate,
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
 * @brief Waits until the worker of source has finished passes more passes, counted by its PollCount.
 *
 * Use it to wait for a worker that polls, such as TailingBytesProducer, instead of sleeping. A pass that was already
 * running when the call began may have looked at the state before the caller changed it, so pass 2 when the worker must
 * have started a whole pass after the change.
 */
template <class Source>
[[nodiscard]] bool WaitForPasses(
    const Source &source, std::size_t passes, std::chrono::milliseconds deadline = DEFAULT_WAIT_DEADLINE
)
{
    const std::size_t target = source.PollCount() + passes;
    return WaitUntil([&] { return source.PollCount() >= target; }, deadline);
}

/**
 * @brief Waits until a thread has said it is about to block, then gives it a moment to block.
 *
 * The thread sets entered just before it calls a blocking function, and nothing can show that it is inside. The short
 * pause makes the test exercise the blocked path and not the path where the wake comes first. The test also passes when
 * the thread has not blocked yet, so the pause only decides which path is measured and is not a correctness wait.
 */
inline void LetWaiterPark(
    const std::atomic<bool> &entered, std::chrono::milliseconds settle = std::chrono::milliseconds{20}
)
{
    static_cast<void>(WaitUntil([&] { return entered.load(std::memory_order_acquire); }));
    std::this_thread::sleep_for(settle);
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
