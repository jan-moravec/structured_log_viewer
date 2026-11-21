#include "common.hpp"

#include <loglib/time/time_input.hpp>
#include <loglib/time/time_zone_context.hpp>

#include <catch2/catch_all.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

using loglib::FormatHasZoneSpecifier;
using loglib::ParseTimeInput;

namespace
{

constexpr std::int64_t REFERENCE_NOW_MICROS = 1'700'000'000'000'000LL;

[[nodiscard]] inline std::chrono::system_clock::time_point ReferenceNow() noexcept
{
    return std::chrono::system_clock::time_point{std::chrono::microseconds{REFERENCE_NOW_MICROS}};
}

} // namespace

TEST_CASE("time_input: relative -Nh / -Nm shortcuts", "[time_input]")
{
    const std::vector<std::string> noFormats;

    const auto oneHourAgo = ParseTimeInput("-1h", noFormats, ReferenceNow());
    REQUIRE(oneHourAgo.has_value());
    REQUIRE(oneHourAgo->microseconds == REFERENCE_NOW_MICROS - (3600LL * 1'000'000LL));
    REQUIRE_FALSE(oneHourAgo->isNaive);

    const auto thirtyMinAgo = ParseTimeInput("-30m", noFormats, ReferenceNow());
    REQUIRE(thirtyMinAgo.has_value());
    REQUIRE(thirtyMinAgo->microseconds == REFERENCE_NOW_MICROS - (30LL * 60LL * 1'000'000LL));
    REQUIRE_FALSE(thirtyMinAgo->isNaive);
}

TEST_CASE("time_input: now is the reference time", "[time_input]")
{
    const std::vector<std::string> noFormats;

    for (const char *text : {"now", "NOW", "  Now "})
    {
        const auto parsed = ParseTimeInput(text, noFormats, ReferenceNow());
        REQUIRE(parsed.has_value());
        CHECK(parsed->microseconds == REFERENCE_NOW_MICROS);
        CHECK_FALSE(parsed->isNaive);
    }
    CHECK_FALSE(ParseTimeInput("nowish", noFormats, ReferenceNow()).has_value());
}

TEST_CASE("time_input: relative +N and bare N mean units ago", "[time_input]")
{
    const std::vector<std::string> noFormats;

    const auto plusOneHour = ParseTimeInput("+1h", noFormats, ReferenceNow());
    REQUIRE(plusOneHour.has_value());
    REQUIRE(plusOneHour->microseconds == REFERENCE_NOW_MICROS - (3600LL * 1'000'000LL));
    REQUIRE_FALSE(plusOneHour->isNaive);

    const auto bareOneHour = ParseTimeInput("1h", noFormats, ReferenceNow());
    REQUIRE(bareOneHour.has_value());
    REQUIRE(bareOneHour->microseconds == REFERENCE_NOW_MICROS - (3600LL * 1'000'000LL));
}

TEST_CASE("time_input: relative shortcuts tolerate whitespace and case", "[time_input]")
{
    const std::vector<std::string> noFormats;
    const auto capitalH = ParseTimeInput("- 2 H", noFormats, ReferenceNow());
    REQUIRE(capitalH.has_value());
    REQUIRE(capitalH->microseconds == REFERENCE_NOW_MICROS - (2LL * 3600LL * 1'000'000LL));

    const auto padded = ParseTimeInput("  + 3 m  ", noFormats, ReferenceNow());
    REQUIRE(padded.has_value());
    REQUIRE(padded->microseconds == REFERENCE_NOW_MICROS - (3LL * 60LL * 1'000'000LL));
}

TEST_CASE("time_input: overflowing relative shortcut is rejected", "[time_input]")
{
    const std::vector<std::string> noFormats;

    REQUIRE_FALSE(ParseTimeInput("-10000000000000000000h", noFormats, ReferenceNow()).has_value());

    const auto hourCap = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() / (3600LL * 1'000'000LL));
    REQUIRE_FALSE(ParseTimeInput(std::to_string(hourCap + 1) + "h", noFormats, ReferenceNow()).has_value());
    REQUIRE(ParseTimeInput(std::to_string(hourCap) + "h", noFormats, ReferenceNow()).has_value());

    const auto minuteCap = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() / (60LL * 1'000'000LL));
    REQUIRE_FALSE(ParseTimeInput(std::to_string(minuteCap + 1) + "m", noFormats, ReferenceNow()).has_value());
    REQUIRE(ParseTimeInput(std::to_string(minuteCap) + "m", noFormats, ReferenceNow()).has_value());
}

TEST_CASE("time_input: column parseFormats are tried before ISO fallbacks", "[time_input]")
{
    const std::vector<std::string> columnFormats{"%Y/%m/%d %H:%M:%S"};
    const auto parsed = ParseTimeInput("2024/04/28 12:34:56", columnFormats, ReferenceNow());
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->microseconds > 0);
    REQUIRE(parsed->isNaive);

    const auto isoT = ParseTimeInput("2024-04-28T12:34:56", {}, ReferenceNow());
    REQUIRE(isoT.has_value());
    REQUIRE(isoT->isNaive);

    const auto isoSpace = ParseTimeInput("2024-04-28 12:34:56", {}, ReferenceNow());
    REQUIRE(isoSpace.has_value());
    REQUIRE(isoT->microseconds == isoSpace->microseconds);
}

TEST_CASE("time_input: zoned formats are not naive", "[time_input]")
{
    const std::vector<std::string> zonedFormats{"%FT%T%Ez"};
    const auto zoned = ParseTimeInput("2024-04-28T12:34:56+02:00", zonedFormats, ReferenceNow());
    REQUIRE(zoned.has_value());
    REQUIRE_FALSE(zoned->isNaive);
}

TEST_CASE("time_input: a trailing Z is not naive", "[time_input]")
{
    const std::vector<std::string> isoFormats{"%FT%T%Ez", "%F %T%Ez", "%FT%T", "%F %T"};
    const auto utc = ParseTimeInput("2024-04-28T12:34:56Z", isoFormats, ReferenceNow());
    REQUIRE(utc.has_value());
    REQUIRE_FALSE(utc->isNaive);

    const auto offset = ParseTimeInput("2024-04-28T12:34:56+00:00", isoFormats, ReferenceNow());
    REQUIRE(offset.has_value());
    REQUIRE(utc->microseconds == offset->microseconds);

    const auto fractional = ParseTimeInput("2024-04-28 12:34:56.5Z", {}, ReferenceNow());
    REQUIRE(fractional.has_value());
    REQUIRE_FALSE(fractional->isNaive);

    const auto naive = ParseTimeInput("2024-04-28T12:34:56", isoFormats, ReferenceNow());
    REQUIRE(naive.has_value());
    REQUIRE(naive->isNaive);
    REQUIRE(naive->microseconds == utc->microseconds);
}

TEST_CASE("time_input: empty and garbage input is rejected", "[time_input]")
{
    REQUIRE_FALSE(ParseTimeInput("", {}, ReferenceNow()).has_value());
    REQUIRE_FALSE(ParseTimeInput("   ", {}, ReferenceNow()).has_value());
    REQUIRE_FALSE(ParseTimeInput("not a timestamp", {}, ReferenceNow()).has_value());
    REQUIRE_FALSE(ParseTimeInput("-1x", {}, ReferenceNow()).has_value());
    REQUIRE_FALSE(ParseTimeInput("--1h", {}, ReferenceNow()).has_value());
}

TEST_CASE("time_input: FormatHasZoneSpecifier scans z, Z, Ez, Oz, and %%", "[time_input]")
{
    REQUIRE_FALSE(FormatHasZoneSpecifier("%FT%T"));
    REQUIRE_FALSE(FormatHasZoneSpecifier("%F %T"));
    REQUIRE_FALSE(FormatHasZoneSpecifier("100%% done"));
    REQUIRE(FormatHasZoneSpecifier("%z"));
    REQUIRE(FormatHasZoneSpecifier("%Z"));
    REQUIRE(FormatHasZoneSpecifier("%Ez"));
    REQUIRE(FormatHasZoneSpecifier("%Oz"));
    REQUIRE(FormatHasZoneSpecifier("%FT%T%Ez"));
    REQUIRE(FormatHasZoneSpecifier("%E%z"));
}

TEST_CASE("time_input: naive parse plus LocalToUtc stays in +/-14h", "[time_input]")
{
    InitializeTimezoneData();

    const auto naiveParse = ParseTimeInput("2024-04-28T12:00:00", {}, ReferenceNow());
    REQUIRE(naiveParse.has_value());
    REQUIRE(naiveParse->isNaive);

    const std::int64_t shifted = TestTimeZone().LocalToUtc(naiveParse->microseconds);
    const std::int64_t delta = shifted - naiveParse->microseconds;
    constexpr std::int64_t MAX_TZ_OFFSET_MICROS = 14LL * 3600LL * 1'000'000LL;
    REQUIRE(std::llabs(delta) <= MAX_TZ_OFFSET_MICROS);

    const std::vector<std::string> zonedFormats{"%FT%T%Ez"};
    const auto zonedParse = ParseTimeInput("2024-04-28T12:00:00+00:00", zonedFormats, ReferenceNow());
    REQUIRE(zonedParse.has_value());
    REQUIRE_FALSE(zonedParse->isNaive);
}

TEST_CASE("time_input: ParseTimeInput does not apply the local-to-UTC shift", "[time_input]")
{
    InitializeTimezoneData();

    const auto naive = ParseTimeInput("2024-04-28T12:00:00", {}, ReferenceNow());
    REQUIRE(naive.has_value());
    REQUIRE(naive->isNaive);
    REQUIRE(ParseTimeInput("2024-04-28T12:00:00", {}, ReferenceNow())->microseconds == naive->microseconds);
    (void)TestTimeZone().LocalToUtc(naive->microseconds);
}
