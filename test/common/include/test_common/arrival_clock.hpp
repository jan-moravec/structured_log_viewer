#pragma once

#include <chrono>
#include <random>

namespace test_common
{

/** @brief Shape of the average arrival rate over a day. */
enum class TrafficProfile
{
    /** Constant rate. */
    Flat,
    /** Sinusoidal day/night cycle, peaking at 14:00 UTC and bottoming out at 02:00 UTC. */
    Daily,
};

/** @brief Tunables for `ArrivalClock`. */
struct ArrivalClockOptions
{
    /** First virtual instant. `Next()` returns times strictly after it. */
    std::chrono::system_clock::time_point start;
    /** Average lines per second; for `Daily` this is the mean over a full day. */
    double linesPerSecond = 20.0;
    TrafficProfile profile = TrafficProfile::Daily;
    /** Whether short high-rate bursts occur at random. */
    bool bursts = false;
};

/**
 * @brief Virtual clock that stamps log lines with realistic arrival times.
 *
 * Gaps between lines are exponentially distributed (a Poisson process) whose
 * rate follows the configured `TrafficProfile` and, when enabled, short
 * bursts. No real time passes, so hours of log can be produced instantly.
 * The output is fully determined by the options and the supplied RNG.
 */
class ArrivalClock
{
public:
    /** @brief Creates a clock positioned at `options.start`. */
    explicit ArrivalClock(const ArrivalClockOptions &options);

    /**
     * @brief Advances to the next arrival and returns its virtual time.
     *
     * @param rng Random source for the gap and burst scheduling.
     */
    std::chrono::system_clock::time_point Next(std::mt19937 &rng);

    /** @brief Returns whether the most recent `Next()` fell inside a burst. */
    bool InBurst() const noexcept
    {
        return mInBurst;
    }

private:
    double RateAt(double secondsSinceEpoch) const;
    void UpdateBurst(double now, std::mt19937 &rng);

    ArrivalClockOptions mOptions;
    double mNow;
    bool mInBurst = false;
    bool mBurstScheduled = false;
    double mBurstStart = 0.0;
    double mBurstEnd = 0.0;
};

} // namespace test_common
