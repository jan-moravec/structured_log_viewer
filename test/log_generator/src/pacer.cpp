#include <log_generator/pacer.hpp>

#include <date/date.h>

#include <sstream>
#include <thread>

namespace log_generator
{

namespace
{

std::string FormatStamp(std::chrono::system_clock::time_point stamp)
{
    return date::format("%FT%T", date::floor<std::chrono::milliseconds>(stamp));
}

} // namespace

Pacer::Pacer(const TimingOptions &timing, std::optional<std::chrono::milliseconds> duration)
    : mSimulated(timing.Simulated()),
      mClockStart(timing.startTime.value_or(std::chrono::system_clock::now())),
      mSteadyStart(std::chrono::steady_clock::now()),
      mDuration(duration),
      mRate(timing.rate.value_or(DEFAULT_SIMULATED_RATE)),
      mProfile(timing.profile),
      mBursts(timing.bursts)
{
    if (mSimulated || timing.rate.has_value())
    {
        mClock.emplace(
            test_common::ArrivalClockOptions{
                .start = mClockStart,
                .linesPerSecond = mRate,
                .profile = mProfile,
                .bursts = mBursts,
            }
        );
    }
}

bool Pacer::Stamp(test_common::LogRecord &record, std::mt19937 &rng)
{
    if (!mClock.has_value())
    {
        return !mDuration.has_value() || std::chrono::steady_clock::now() - mSteadyStart < *mDuration;
    }

    const auto stamp = mClock->Next(rng);
    if (mDuration.has_value() && stamp >= mClockStart + *mDuration)
    {
        return false;
    }
    mLastStamp = stamp;
    record["timestamp"] = FormatStamp(stamp);
    if (mClock->InBurst())
    {
        // Bursts model incidents: mostly warnings and errors.
        std::uniform_int_distribution<int> pick(0, 9);
        const int roll = pick(rng);
        if (roll < 5)
        {
            record["level"] = std::string("error");
        }
        else if (roll < 8)
        {
            record["level"] = std::string("warning");
        }
    }
    return true;
}

void Pacer::WaitForStamp() const
{
    if (!Paced() || !mLastStamp.has_value())
    {
        return;
    }
    const auto offset = std::chrono::duration_cast<std::chrono::steady_clock::duration>(*mLastStamp - mClockStart);
    std::this_thread::sleep_until(mSteadyStart + offset);
}

std::optional<std::chrono::system_clock::time_point> Pacer::SimulatedStart() const
{
    return mSimulated ? std::optional(mClockStart) : std::nullopt;
}

std::optional<std::chrono::system_clock::time_point> Pacer::SimulatedEnd() const
{
    return mSimulated ? mLastStamp : std::nullopt;
}

std::string Pacer::Describe() const
{
    if (!mClock.has_value())
    {
        return "unthrottled";
    }
    std::ostringstream out;
    if (mSimulated)
    {
        out << "simulated from " << FormatStamp(mClockStart) << "Z, ";
    }
    out << mRate << " lines/s avg, profile=" << (mProfile == test_common::TrafficProfile::Flat ? "flat" : "daily")
        << (mBursts ? ", bursts" : "");
    return out.str();
}

} // namespace log_generator
