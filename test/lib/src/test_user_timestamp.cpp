#include "common.hpp"

#include <loglib/log_processing.hpp>
#include <loglib/user_timestamp.hpp>

#include <catch2/catch_all.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

using loglib::FormatHasZoneSpecifier;
using loglib::LocalMicrosecondsSinceEpochToUtc;
using loglib::ParseUserTimestamp;

namespace
{

constexpr std::int64_t REFERENCE_NOW_MICROS = 1'700'000'000'000'000LL;

[[nodiscard]] inline std::chrono::system_clock::time_point ReferenceNow() noexcept
{
    return std::chrono::system_clock::time_point{std::chrono::microseconds{REFERENCE_NOW_MICROS}};
}

} // namespace

TEST_CASE("user_timestamp: relative -Nh / -Nm shortcuts", "[user_timestamp]")
{
    const std::vector<std::string> noFormats;

    const auto oneHourAgo = ParseUserTimestamp("-1h", noFormats, ReferenceNow());
    REQUIRE(oneHourAgo.has_value());
    REQUIRE(oneHourAgo->micros == REFERENCE_NOW_MICROS - (3600LL * 1'000'000LL));
    REQUIRE_FALSE(oneHourAgo->isNaive);

    const auto thirtyMinAgo = ParseUserTimestamp("-30m", noFormats, ReferenceNow());
    REQUIRE(thirtyMinAgo.has_value());
    REQUIRE(thirtyMinAgo->micros == REFERENCE_NOW_MICROS - (30LL * 60LL * 1'000'000LL));
    REQUIRE_FALSE(thirtyMinAgo->isNaive);
}

TEST_CASE("user_timestamp: relative +N and bare N mean units ago", "[user_timestamp]")
{
    const std::vector<std::string> noFormats;

    const auto plusOneHour = ParseUserTimestamp("+1h", noFormats, ReferenceNow());
    REQUIRE(plusOneHour.has_value());
    REQUIRE(plusOneHour->micros == REFERENCE_NOW_MICROS - (3600LL * 1'000'000LL));
    REQUIRE_FALSE(plusOneHour->isNaive);

    const auto bareOneHour = ParseUserTimestamp("1h", noFormats, ReferenceNow());
    REQUIRE(bareOneHour.has_value());
    REQUIRE(bareOneHour->micros == REFERENCE_NOW_MICROS - (3600LL * 1'000'000LL));
}

TEST_CASE("user_timestamp: relative shortcuts tolerate whitespace and case", "[user_timestamp]")
{
    const std::vector<std::string> noFormats;
    const auto capitalH = ParseUserTimestamp("- 2 H", noFormats, ReferenceNow());
    REQUIRE(capitalH.has_value());
    REQUIRE(capitalH->micros == REFERENCE_NOW_MICROS - (2LL * 3600LL * 1'000'000LL));

    const auto padded = ParseUserTimestamp("  + 3 m  ", noFormats, ReferenceNow());
    REQUIRE(padded.has_value());
    REQUIRE(padded->micros == REFERENCE_NOW_MICROS - (3LL * 60LL * 1'000'000LL));
}

TEST_CASE("user_timestamp: overflowing relative shortcut is rejected", "[user_timestamp]")
{
    const std::vector<std::string> noFormats;

    REQUIRE_FALSE(ParseUserTimestamp("-10000000000000000000h", noFormats, ReferenceNow()).has_value());

    const auto hourCap = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() / (3600LL * 1'000'000LL));
    REQUIRE_FALSE(ParseUserTimestamp(std::to_string(hourCap + 1) + "h", noFormats, ReferenceNow()).has_value());
    REQUIRE(ParseUserTimestamp(std::to_string(hourCap) + "h", noFormats, ReferenceNow()).has_value());

    const auto minuteCap = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() / (60LL * 1'000'000LL));
    REQUIRE_FALSE(ParseUserTimestamp(std::to_string(minuteCap + 1) + "m", noFormats, ReferenceNow()).has_value());
    REQUIRE(ParseUserTimestamp(std::to_string(minuteCap) + "m", noFormats, ReferenceNow()).has_value());
}

TEST_CASE("user_timestamp: column parseFormats are tried before ISO fallbacks", "[user_timestamp]")
{
    const std::vector<std::string> columnFormats{"%Y/%m/%d %H:%M:%S"};
    const auto parsed = ParseUserTimestamp("2024/04/28 12:34:56", columnFormats, ReferenceNow());
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->micros > 0);
    REQUIRE(parsed->isNaive);

    const auto isoT = ParseUserTimestamp("2024-04-28T12:34:56", {}, ReferenceNow());
    REQUIRE(isoT.has_value());
    REQUIRE(isoT->isNaive);

    const auto isoSpace = ParseUserTimestamp("2024-04-28 12:34:56", {}, ReferenceNow());
    REQUIRE(isoSpace.has_value());
    REQUIRE(isoT->micros == isoSpace->micros);
}

TEST_CASE("user_timestamp: zoned formats are not naive", "[user_timestamp]")
{
    const std::vector<std::string> zonedFormats{"%FT%T%Ez"};
    const auto zoned = ParseUserTimestamp("2024-04-28T12:34:56+02:00", zonedFormats, ReferenceNow());
    REQUIRE(zoned.has_value());
    REQUIRE_FALSE(zoned->isNaive);
}

TEST_CASE("user_timestamp: empty and garbage input is rejected", "[user_timestamp]")
{
    REQUIRE_FALSE(ParseUserTimestamp("", {}, ReferenceNow()).has_value());
    REQUIRE_FALSE(ParseUserTimestamp("   ", {}, ReferenceNow()).has_value());
    REQUIRE_FALSE(ParseUserTimestamp("not a timestamp", {}, ReferenceNow()).has_value());
    REQUIRE_FALSE(ParseUserTimestamp("-1x", {}, ReferenceNow()).has_value());
    REQUIRE_FALSE(ParseUserTimestamp("--1h", {}, ReferenceNow()).has_value());
}

TEST_CASE("user_timestamp: FormatHasZoneSpecifier scans z, Z, Ez, Oz, and %%", "[user_timestamp]")
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

TEST_CASE("user_timestamp: naive parse plus LocalMicrosecondsSinceEpochToUtc stays in +/-14h", "[user_timestamp]")
{
    InitializeTimezoneData();

    const auto naiveParse = ParseUserTimestamp("2024-04-28T12:00:00", {}, ReferenceNow());
    REQUIRE(naiveParse.has_value());
    REQUIRE(naiveParse->isNaive);

    const std::int64_t shifted = LocalMicrosecondsSinceEpochToUtc(naiveParse->micros);
    const std::int64_t delta = shifted - naiveParse->micros;
    constexpr std::int64_t MAX_TZ_OFFSET_MICROS = 14LL * 3600LL * 1'000'000LL;
    REQUIRE(std::llabs(delta) <= MAX_TZ_OFFSET_MICROS);

    const std::vector<std::string> zonedFormats{"%FT%T%Ez"};
    const auto zonedParse = ParseUserTimestamp("2024-04-28T12:00:00+00:00", zonedFormats, ReferenceNow());
    REQUIRE(zonedParse.has_value());
    REQUIRE_FALSE(zonedParse->isNaive);
}

TEST_CASE("user_timestamp: ParseUserTimestamp does not apply the local-to-UTC shift", "[user_timestamp]")
{
    InitializeTimezoneData();

    const auto naive = ParseUserTimestamp("2024-04-28T12:00:00", {}, ReferenceNow());
    REQUIRE(naive.has_value());
    REQUIRE(naive->isNaive);
    REQUIRE(ParseUserTimestamp("2024-04-28T12:00:00", {}, ReferenceNow())->micros == naive->micros);
    (void)LocalMicrosecondsSinceEpochToUtc(naive->micros);
}
