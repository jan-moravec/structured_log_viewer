#include "common.hpp"

#include <loglib/core/log_value.hpp>
#include <loglib/time/internal/timestamp_formatter.hpp>
#include <loglib/time/time_zone_context.hpp>

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <utility>
#include <vector>

using namespace loglib;
using loglib::internal::TimestampFormatter;

namespace
{

constexpr std::int64_t MICROS_PER_SECOND = 1'000'000;
constexpr std::int64_t MICROS_PER_DAY = 86'400 * MICROS_PER_SECOND;

/** @brief The text of the general formatter, or a marker when it throws, so both sides can be compared. */
std::string Reference(
    const TimeZoneContext &zone, TimeStamp time, const std::string &format, FractionPrecision precision
)
{
    try
    {
        return zone.Format(time, format, precision);
    }
    catch (const std::exception &)
    {
        return "<throws>";
    }
}

std::string Fast(TimestampFormatter &formatter, TimeStamp time)
{
    try
    {
        return formatter.Format(time);
    }
    catch (const std::exception &)
    {
        return "<throws>";
    }
}

/** @brief Instants that are likely to break a formatter: the epoch, day and year ends, leap days, far ends. */
std::vector<std::int64_t> EdgeInstants()
{
    std::vector<std::int64_t> instants{0, 1, -1, 999, 1000, 499, 500, 501, 1500, -500, -501, -1500};
    // 0000-01-01, 0001-01-01, 1969-12-31, 2000-02-29, 2024-02-29, 2100-02-28, 2100-03-01 and 9999-12-31 in days from
    // the epoch, and the day before 0000.
    for (const std::int64_t day : {-719'528, -719'162, -1, 11'016, 19'782, 47'541, 47'542, 2'932'896, -719'529})
    {
        for (const std::int64_t offset :
             {-2 * MICROS_PER_DAY,
              -MICROS_PER_SECOND,
              std::int64_t{-1},
              std::int64_t{0},
              std::int64_t{1},
              MICROS_PER_SECOND - 1,
              MICROS_PER_DAY - 1,
              MICROS_PER_DAY})
        {
            instants.push_back(day * MICROS_PER_DAY + offset);
        }
    }
    instants.push_back(std::numeric_limits<std::int64_t>::min());
    instants.push_back(std::numeric_limits<std::int64_t>::max());
    return instants;
}

/** @brief Instants around every clock change of a zone in a few years, to the microsecond. */
std::vector<std::int64_t> TransitionInstants(const TimeZoneContext &zone)
{
    std::vector<std::int64_t> instants;
    std::int64_t at = 1'577'836'800 * MICROS_PER_SECOND;        // 2020-01-01
    const std::int64_t end = 1'735'689'600 * MICROS_PER_SECOND; // 2025-01-01
    while (at < end)
    {
        const auto span = zone.ZoneSpanAt(at);
        REQUIRE(span.has_value());
        if (span->endUtc >= end)
        {
            break;
        }
        for (const std::int64_t delta :
             {-3'600 * MICROS_PER_SECOND,
              -MICROS_PER_SECOND,
              std::int64_t{-1},
              std::int64_t{0},
              std::int64_t{1},
              MICROS_PER_SECOND})
        {
            instants.push_back(span->endUtc + delta);
        }
        at = span->endUtc;
    }
    return instants;
}

} // namespace

TEST_CASE("The timestamp formatter writes the same text as the general formatter", "[timestamp_formatter]")
{
    InitializeTimezoneData();
    const std::vector<std::pair<std::string, TimeZoneContext>> zones{
        {"UTC", TimeZoneContext::Utc()},
        {"Europe/Berlin", TimeZoneContext::Load(FindTestTzdata(), "Europe/Berlin")},
        {"America/New_York", TimeZoneContext::Load(FindTestTzdata(), "America/New_York")},
        {"Asia/Kolkata", TimeZoneContext::Load(FindTestTzdata(), "Asia/Kolkata")},
        {"Australia/Lord_Howe", TimeZoneContext::Load(FindTestTzdata(), "Australia/Lord_Howe")},
        {"Pacific/Kiritimati", TimeZoneContext::Load(FindTestTzdata(), "Pacific/Kiritimati")},
    };
    const std::vector<std::string> formats{
        "%F %T",
        "%FT%T",
        "%FT%T%z",
        "%FT%T%Ez",
        "%Y-%m-%d %H:%M:%S",
        "%d.%m.%Y %H:%M",
        "[%Z] %F %T %z %Ez %Z",
        "%H:%M:%S",
        "plain text",
        "",
        // Formats outside the steps use the general formatter.
        "%a %b %e %H:%M:%S %Y",
        "%FT%T%%",
        "%j",
        "%Q",
        "%",
    };

    std::mt19937_64 generator(7);
    std::vector<std::int64_t> instants = EdgeInstants();
    for (int i = 0; i < 600; ++i)
    {
        // Years from 0000 to a little past 9999, as microseconds.
        instants.push_back(
            std::uniform_int_distribution<std::int64_t>(-62'200'000'000'000'000, 254'000'000'000'000'000)(generator)
        );
    }
    for (int i = 0; i < 600; ++i)
    {
        // The years that logs hold, so the day text is reused between neighbours.
        instants.push_back(
            std::uniform_int_distribution<std::int64_t>(0, 4'102'444'800 * MICROS_PER_SECOND)(generator)
        );
    }

    for (const auto &[zoneName, zone] : zones)
    {
        std::vector<std::int64_t> zoneInstants = instants;
        if (!zone.IsUtc())
        {
            const std::vector<std::int64_t> transitions = TransitionInstants(zone);
            zoneInstants.insert(zoneInstants.end(), transitions.begin(), transitions.end());
        }
        // Neighbouring instants, in order, exercise the day and zone span reuse.
        std::ranges::sort(zoneInstants);

        for (const std::string &format : formats)
        {
            for (const FractionPrecision precision : {FractionPrecision::Milliseconds, FractionPrecision::Microseconds})
            {
                TimestampFormatter formatter(zone, format, precision);
                INFO("zone " << zoneName << " format '" << format << "'");
                for (const std::int64_t micros : zoneInstants)
                {
                    const TimeStamp time{std::chrono::microseconds{micros}};
                    const std::string expected = Reference(zone, time, format, precision);
                    const std::string actual = Fast(formatter, time);
                    if (actual != expected)
                    {
                        FAIL("micros " << micros << ": expected '" << expected << "', got '" << actual << "'");
                    }
                }
            }
        }
    }
}

TEST_CASE("The timestamp formatter is right when times arrive out of order", "[timestamp_formatter]")
{
    InitializeTimezoneData();
    const TimeZoneContext berlin = TimeZoneContext::Load(FindTestTzdata(), "Europe/Berlin");
    std::vector<std::int64_t> instants = TransitionInstants(berlin);
    std::mt19937_64 generator(3);
    for (int i = 0; i < 300; ++i)
    {
        instants.push_back(
            std::uniform_int_distribution<std::int64_t>(0, 4'102'444'800 * MICROS_PER_SECOND)(generator)
        );
    }
    std::ranges::shuffle(instants, generator);

    TimestampFormatter formatter(berlin, "%F %T %Z %z", FractionPrecision::Milliseconds);
    for (const std::int64_t micros : instants)
    {
        const TimeStamp time{std::chrono::microseconds{micros}};
        const std::string expected = Reference(berlin, time, "%F %T %Z %z", FractionPrecision::Milliseconds);
        const std::string actual = Fast(formatter, time);
        if (actual != expected)
        {
            FAIL("micros " << micros << ": expected '" << expected << "', got '" << actual << "'");
        }
    }
}

TEST_CASE("The timestamp formatter appends to the text it is given", "[timestamp_formatter]")
{
    TimestampFormatter formatter(TimeZoneContext::Utc(), "%FT%T", FractionPrecision::Microseconds);
    std::string text = "time=";
    formatter.Append(text, TimeStamp{std::chrono::microseconds{1'767'225'600'123'456}});
    formatter.Append(text, TimeStamp{});
    CHECK(text == "time=2026-01-01T00:00:00.1234561970-01-01T00:00:00.000000");
}

TEST_CASE("Time zone context formatting uses the same text as the general formatter", "[timestamp_formatter]")
{
    InitializeTimezoneData();
    const TimeZoneContext berlin = TimeZoneContext::Load(FindTestTzdata(), "Europe/Berlin");
    const TimeZoneContext utc = TimeZoneContext::Utc();
    std::mt19937_64 generator(11);
    for (int i = 0; i < 2000; ++i)
    {
        const TimeStamp time{std::chrono::microseconds{
            std::uniform_int_distribution<std::int64_t>(0, 4'102'444'800 * MICROS_PER_SECOND)(generator)
        }};
        // Alternate zones and formats, so the per-thread formatter is replaced and reused.
        const TimeZoneContext &zone = i % 3 == 0 ? utc : berlin;
        const std::string format = i % 5 == 0 ? "%FT%T%z" : "%F %T";
        REQUIRE(zone.Format(time, format) == zone.Format(time, format, FractionPrecision::Milliseconds));
    }
}
