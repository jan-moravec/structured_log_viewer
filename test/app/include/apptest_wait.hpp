/**
 * @brief Polling wait for GUI tests that lets the Qt event loop run while it waits.
 *
 * WaitUntilProcessingEvents calls a predicate until it holds or a deadline passes, and processes pending Qt events
 * between calls, so signals, queued calls and timers that the waited-for condition depends on can run. The deadline
 * only guards against a hang, so it is generous and a passing test returns as soon as the condition holds. A test that
 * must show that nothing happens uses WaitToProveAbsence, the one place a GUI test waits a fixed time.
 *
 * It is the Qt counterpart of the wait helpers in test_common, which is Qt-free, and is part of the apptest_support
 * library.
 */
#pragma once

#include <QCoreApplication>
#include <QEventLoop>
#include <QTest>

#include <chrono>

namespace apptest_support
{

/** @brief The deadline of WaitUntilProcessingEvents when the caller gives none. It guards a hang and is never a speed claim. */
inline constexpr std::chrono::milliseconds DEFAULT_EVENT_WAIT_DEADLINE{60000};

/**
 * @brief Calls predicate until it returns true or deadline passes, processing Qt events between calls.
 *
 * Returns whether the predicate held. It is checked once more at the deadline, so a condition that became true during
 * the last round of events is not missed. Use QTRY_VERIFY_WITH_TIMEOUT instead where a check follows the wait.
 */
template <class Predicate>
[[nodiscard]] bool WaitUntilProcessingEvents(
    Predicate predicate, std::chrono::milliseconds deadline = DEFAULT_EVENT_WAIT_DEADLINE
)
{
    const auto giveUpAt = std::chrono::steady_clock::now() + deadline;
    while (!predicate())
    {
        if (std::chrono::steady_clock::now() >= giveUpAt)
        {
            return predicate();
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QTest::qWait(1);
    }
    return true;
}

/**
 * @brief Waits a fixed time with the event loop running, so a test can check that something did not happen.
 *
 * Polling cannot show an absence, so time is the only signal. Use it only where nothing observable marks the end of
 * the period, keep the time short, and say in the calling test what the quiet period proves.
 */
inline void WaitToProveAbsence(std::chrono::milliseconds quietPeriod)
{
    QTest::qWait(static_cast<int>(quietPeriod.count()));
}

} // namespace apptest_support
