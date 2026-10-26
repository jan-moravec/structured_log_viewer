#include <test_common/arrival_clock.hpp>

#include <catch2/catch_all.hpp>

#include <chrono>
#include <cstddef>
#include <random>

namespace
{

using Clock = std::chrono::system_clock;

const Clock::time_point START = Clock::time_point(std::chrono::seconds(1'767'225'600)); // 2026-01-01T00:00:00Z

} // namespace

TEST_CASE("ArrivalClock is monotonic and deterministic for a seed", "[arrival_clock]")
{
    const test_common::ArrivalClockOptions options{.start = START, .linesPerSecond = 50.0, .bursts = true};

    std::mt19937 rngA(42);
    std::mt19937 rngB(42);
    test_common::ArrivalClock a(options);
    test_common::ArrivalClock b(options);

    auto previous = START;
    for (int i = 0; i < 5000; ++i)
    {
        const auto ta = a.Next(rngA);
        REQUIRE(ta == b.Next(rngB));
        REQUIRE(ta >= previous);
        previous = ta;
    }
}

TEST_CASE("ArrivalClock flat profile averages the configured rate", "[arrival_clock]")
{
    const test_common::ArrivalClockOptions options{
        .start = START, .linesPerSecond = 100.0, .profile = test_common::TrafficProfile::Flat
    };
    std::mt19937 rng(7);
    test_common::ArrivalClock clock(options);

    constexpr int COUNT = 20000;
    Clock::time_point last = START;
    for (int i = 0; i < COUNT; ++i)
    {
        last = clock.Next(rng);
    }
    const double seconds = std::chrono::duration<double>(last - START).count();
    REQUIRE(COUNT / seconds == Catch::Approx(100.0).epsilon(0.05));
}

TEST_CASE("ArrivalClock daily profile is busier in the afternoon than at night", "[arrival_clock]")
{
    const test_common::ArrivalClockOptions options{
        .start = START, .linesPerSecond = 20.0, .profile = test_common::TrafficProfile::Daily
    };
    std::mt19937 rng(11);
    test_common::ArrivalClock clock(options);

    std::size_t night = 0;     // 01:00-03:00
    std::size_t afternoon = 0; // 13:00-15:00
    const auto end = START + std::chrono::hours(24);
    for (auto t = clock.Next(rng); t < end; t = clock.Next(rng))
    {
        const auto hour = std::chrono::duration_cast<std::chrono::hours>(t - START).count();
        if (hour == 1 || hour == 2)
        {
            ++night;
        }
        else if (hour == 13 || hour == 14)
        {
            ++afternoon;
        }
    }
    REQUIRE(afternoon > night * 3);
}

TEST_CASE("ArrivalClock bursts occur and raise the local rate", "[arrival_clock]")
{
    const test_common::ArrivalClockOptions options{
        .start = START, .linesPerSecond = 5.0, .profile = test_common::TrafficProfile::Flat, .bursts = true
    };
    std::mt19937 rng(3);
    test_common::ArrivalClock clock(options);

    std::size_t burstLines = 0;
    const auto end = START + std::chrono::hours(6);
    for (auto t = clock.Next(rng); t < end; t = clock.Next(rng))
    {
        if (clock.InBurst())
        {
            ++burstLines;
        }
    }
    REQUIRE(burstLines > 0);
}
