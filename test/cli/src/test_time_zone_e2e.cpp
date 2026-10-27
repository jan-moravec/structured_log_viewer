#include "slp_process.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <vector>

using slp::test::CommandResult;
using slp::test::FixturesDir;
using slp::test::RunSlp;
using slp::test::SlpBinary;

namespace
{

// Asia/Tokyo is UTC+9 all year, so results never depend on the machine zone or on DST.
constexpr const char *FIXED_ZONE = "Asia/Tokyo";

[[nodiscard]] bool TzdataStaged()
{
    return std::filesystem::is_directory(SlpBinary().parent_path() / "tzdata");
}

[[nodiscard]] CommandResult RunNaiveTimes(const std::string &zone, std::vector<std::string> extra)
{
    std::vector<std::string> args = {
        "query", "--tz", zone, "--template-dir", (FixturesDir() / "empty_templates").string()
    };
    args.insert(args.end(), extra.begin(), extra.end());
    args.push_back((FixturesDir() / "naive_times.jsonl").string());
    return RunSlp(args);
}

[[nodiscard]] std::string CountInWindow(const std::string &zone, const std::string &since, const std::string &until)
{
    const auto result = RunNaiveTimes(zone, {"--count", "--allow-empty", "--since", since, "--until", until});
    INFO("stderr:\n" << result.err);
    REQUIRE(result.exitCode == 0);
    return result.out;
}

} // namespace

TEST_CASE("slp --since/--until read a trailing Z as UTC whatever --tz says", "[cli][e2e][time]")
{
    if (!TzdataStaged())
    {
        SKIP("tzdata is not staged next to slp");
    }
    for (const std::string zone : {"UTC", FIXED_ZONE})
    {
        INFO("--tz " << zone);
        CHECK(CountInWindow(zone, "2026-03-01T13:00:00Z", "2026-03-01T14:00:00Z") == "3\n");
        CHECK(CountInWindow(zone, "2026-03-01T13:00:00+00:00", "2026-03-01T14:00:00+00:00") == "3\n");
        CHECK(CountInWindow(zone, "2026-03-01 13:00:00Z", "2026-03-01 14:00:00Z") == "3\n");
    }
}

TEST_CASE("slp --since/--until read a value without a zone in --tz", "[cli][e2e][time]")
{
    if (!TzdataStaged())
    {
        SKIP("tzdata is not staged next to slp");
    }
    CHECK(CountInWindow("UTC", "2026-03-01T13:00:00", "2026-03-01T14:00:00") == "3\n");
    // 22:00-23:00 in Tokyo is 13:00-14:00 UTC.
    CHECK(CountInWindow(FIXED_ZONE, "2026-03-01T22:00:00", "2026-03-01T23:00:00") == "3\n");
    CHECK(CountInWindow(FIXED_ZONE, "2026-03-01T13:00:00", "2026-03-01T14:00:00") == "0\n");
}

TEST_CASE("slp reads naive log timestamps as UTC and renders them in --tz", "[cli][e2e][time]")
{
    if (!TzdataStaged())
    {
        SKIP("tzdata is not staged next to slp");
    }
    const std::vector<std::string> firstTimestamp = {
        "--limit", "1", "--columns", "timestamp", "-t", "csv", "--no-header"
    };

    const auto utc = RunNaiveTimes("UTC", firstTimestamp);
    INFO("stderr:\n" << utc.err);
    REQUIRE(utc.exitCode == 0);
    CHECK(utc.out == "2026-03-01 00:00:00.082\n");

    const auto tokyo = RunNaiveTimes(FIXED_ZONE, firstTimestamp);
    INFO("stderr:\n" << tokyo.err);
    REQUIRE(tokyo.exitCode == 0);
    CHECK(tokyo.out == "2026-03-01 09:00:00.082\n");

    // A UTC filter bound matches the naive log value with no --tz shift.
    const auto zonedFilter =
        RunNaiveTimes(FIXED_ZONE, {"--count", "--filter", "timestamp IN [2026-03-01T00:00:00Z..2026-03-01T00:00:01Z]"});
    INFO("stderr:\n" << zonedFilter.err);
    REQUIRE(zonedFilter.exitCode == 0);
    CHECK(zonedFilter.out == "1\n");
}
