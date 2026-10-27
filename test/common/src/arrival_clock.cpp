#include <test_common/arrival_clock.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace test_common
{

namespace
{

constexpr double SECONDS_PER_DAY = 86400.0;
constexpr double DAILY_PEAK_HOUR = 14.0;
constexpr double DAILY_AMPLITUDE = 0.8;
constexpr double BURST_RATE_MULTIPLIER = 10.0;
constexpr double BURST_MEAN_SPACING_SECONDS = 600.0;
constexpr double BURST_MIN_SECONDS = 5.0;
constexpr double BURST_MAX_SECONDS = 45.0;
constexpr double MIN_RATE = 1e-6;

double ToSeconds(std::chrono::system_clock::time_point tp)
{
    return std::chrono::duration<double>(tp.time_since_epoch()).count();
}

std::chrono::system_clock::time_point FromSeconds(double seconds)
{
    return std::chrono::system_clock::time_point(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::duration<double>(seconds))
    );
}

double SampleExponential(std::mt19937 &rng, double rate)
{
    std::exponential_distribution<double> dist(std::max(rate, MIN_RATE));
    return dist(rng);
}

} // namespace

ArrivalClock::ArrivalClock(const ArrivalClockOptions &options)
    : mOptions(options), mNow(ToSeconds(options.start))
{
}

double ArrivalClock::RateAt(double secondsSinceEpoch) const
{
    double rate = mOptions.linesPerSecond;
    if (mOptions.profile == TrafficProfile::Daily)
    {
        const double hour = std::fmod(secondsSinceEpoch, SECONDS_PER_DAY) / 3600.0;
        rate *= 1.0 + (DAILY_AMPLITUDE * std::cos(2.0 * std::numbers::pi * (hour - DAILY_PEAK_HOUR) / 24.0));
    }
    if (mInBurst)
    {
        rate *= BURST_RATE_MULTIPLIER;
    }
    return rate;
}

void ArrivalClock::UpdateBurst(double now, std::mt19937 &rng)
{
    const auto schedule = [&](double after) {
        std::uniform_real_distribution<double> length(BURST_MIN_SECONDS, BURST_MAX_SECONDS);
        mBurstStart = after + SampleExponential(rng, 1.0 / BURST_MEAN_SPACING_SECONDS);
        mBurstEnd = mBurstStart + length(rng);
    };

    if (!mBurstScheduled)
    {
        schedule(now);
        mBurstScheduled = true;
    }
    while (now >= mBurstEnd)
    {
        schedule(mBurstEnd);
    }
    mInBurst = now >= mBurstStart;
}

std::chrono::system_clock::time_point ArrivalClock::Next(std::mt19937 &rng)
{
    if (mOptions.bursts)
    {
        UpdateBurst(mNow, rng);
    }
    mNow += SampleExponential(rng, RateAt(mNow));
    if (mOptions.bursts)
    {
        UpdateBurst(mNow, rng);
    }
    return FromSeconds(mNow);
}

} // namespace test_common
