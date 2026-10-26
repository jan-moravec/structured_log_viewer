#pragma once

#include <log_generator/options.hpp>

#include <test_common/arrival_clock.hpp>
#include <test_common/log_record.hpp>

#include <chrono>
#include <optional>
#include <random>
#include <string>

namespace log_generator
{

/**
 * @brief Stamps records with timestamps and paces how fast they are written.
 *
 * Without `TimingOptions::rate` or `TimingOptions::startTime`, records keep the
 * wall-clock timestamp from the record generator and nothing waits. With a
 * rate, an `ArrivalClock` starting now stamps each record and `WaitForStamp()`
 * sleeps until that time. With a start time, the `ArrivalClock` runs from it
 * and nothing waits. During a burst, most records become warnings or errors.
 */
class Pacer
{
public:
    /**
     * @brief Creates a pacer whose clock starts now.
     *
     * @param timing Timing settings.
     * @param duration Time span after which `Stamp()` reports the end, if any.
     */
    Pacer(const TimingOptions &timing, std::optional<std::chrono::milliseconds> duration);

    /**
     * @brief Assigns the next timestamp to @p record.
     *
     * @param record Record to stamp; its `level` may change during a burst.
     * @param rng Random source shared with the record generator.
     * @return `false` once the duration limit is reached; @p record is then not stamped.
     */
    bool Stamp(test_common::LogRecord &record, std::mt19937 &rng);

    /** @brief Returns whether `WaitForStamp()` sleeps, i.e. whether a live rate is set. */
    bool Paced() const noexcept
    {
        return mClock.has_value() && !mSimulated;
    }

    /** @brief Sleeps until the wall clock reaches the most recent stamp. Returns at once unless `Paced()`. */
    void WaitForStamp() const;

    /** @brief Returns the virtual start time of a simulated run. */
    std::optional<std::chrono::system_clock::time_point> SimulatedStart() const;

    /** @brief Returns the most recent stamp of a simulated run. */
    std::optional<std::chrono::system_clock::time_point> SimulatedEnd() const;

    /** @brief Returns a one-line summary such as `5 lines/s avg, profile=daily, bursts`. */
    std::string Describe() const;

private:
    bool mSimulated;
    std::optional<test_common::ArrivalClock> mClock;
    std::chrono::system_clock::time_point mClockStart;
    std::chrono::steady_clock::time_point mSteadyStart;
    std::optional<std::chrono::milliseconds> mDuration;
    std::optional<std::chrono::system_clock::time_point> mLastStamp;
    double mRate;
    test_common::TrafficProfile mProfile;
    bool mBursts;
};

} // namespace log_generator
