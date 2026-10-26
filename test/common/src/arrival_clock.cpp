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
    : m_options(options), m_now(ToSeconds(options.start))
{
}

double ArrivalClock::RateAt(double secondsSinceEpoch) const
{
    double rate = m_options.linesPerSecond;
    if (m_options.profile == TrafficProfile::Daily)
    {
        const double hour = std::fmod(secondsSinceEpoch, SECONDS_PER_DAY) / 3600.0;
        rate *= 1.0 + (DAILY_AMPLITUDE * std::cos(2.0 * std::numbers::pi * (hour - DAILY_PEAK_HOUR) / 24.0));
    }
    if (m_inBurst)
    {
        rate *= BURST_RATE_MULTIPLIER;
    }
    return rate;
}

void ArrivalClock::UpdateBurst(double now, std::mt19937 &rng)
{
    const auto schedule = [&](double after) {
        std::uniform_real_distribution<double> length(BURST_MIN_SECONDS, BURST_MAX_SECONDS);
        m_burstStart = after + SampleExponential(rng, 1.0 / BURST_MEAN_SPACING_SECONDS);
        m_burstEnd = m_burstStart + length(rng);
    };

    if (!m_burstScheduled)
    {
        schedule(now);
        m_burstScheduled = true;
    }
    while (now >= m_burstEnd)
    {
        schedule(m_burstEnd);
    }
    m_inBurst = now >= m_burstStart;
}

std::chrono::system_clock::time_point ArrivalClock::Next(std::mt19937 &rng)
{
    if (m_options.bursts)
    {
        UpdateBurst(m_now, rng);
    }
    m_now += SampleExponential(rng, RateAt(m_now));
    if (m_options.bursts)
    {
        UpdateBurst(m_now, rng);
    }
    return FromSeconds(m_now);
}

} // namespace test_common
