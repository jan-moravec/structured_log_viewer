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

[[nodiscard]] std::string CountMatching(const std::string &zone, const std::string &filter)
{
    const auto result = RunNaiveTimes(zone, {"--count", "--allow-empty", "--filter", filter});
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
    CHECK(CountInWindow("UTC", "2026-03-01T13:00:00Z", "2026-03-01T14:00:00Z") == "3\n");
    CHECK(CountInWindow("UTC", "2026-03-01T13:00:00+00:00", "2026-03-01T14:00:00+00:00") == "3\n");
    CHECK(CountInWindow("UTC", "2026-03-01 13:00:00Z", "2026-03-01 14:00:00Z") == "3\n");

    // The log has no zones, so under Tokyo its 13:00-14:00 is 04:00-05:00 UTC.
    CHECK(CountInWindow(FIXED_ZONE, "2026-03-01T04:00:00Z", "2026-03-01T05:00:00Z") == "3\n");
    CHECK(CountInWindow(FIXED_ZONE, "2026-03-01T04:00:00+00:00", "2026-03-01T05:00:00+00:00") == "3\n");
    CHECK(CountInWindow(FIXED_ZONE, "2026-03-01T13:00:00Z", "2026-03-01T14:00:00Z") == "0\n");
}

TEST_CASE("slp --since/--until read a value without a zone in --tz", "[cli][e2e][time]")
{
    if (!TzdataStaged())
    {
        SKIP("tzdata is not staged next to slp");
    }
    for (const std::string zone : {"UTC", FIXED_ZONE})
    {
        INFO("--tz " << zone);
        CHECK(CountInWindow(zone, "2026-03-01T13:00:00", "2026-03-01T14:00:00") == "3\n");
        CHECK(CountInWindow(zone, "2026-03-01T22:00:00", "2026-03-01T23:00:00") == "0\n");
    }
}

TEST_CASE("slp --filter bounds without a zone agree with --since", "[cli][e2e][time]")
{
    if (!TzdataStaged())
    {
        SKIP("tzdata is not staged next to slp");
    }
    for (const std::string zone : {"UTC", FIXED_ZONE})
    {
        INFO("--tz " << zone);
        CHECK(CountMatching(zone, "timestamp IN [2026-03-01T13:00:00..2026-03-01T14:00:00]") == "3\n");
    }
    CHECK(CountMatching(FIXED_ZONE, "timestamp IN [2026-03-01T04:00:00Z..2026-03-01T05:00:00Z]") == "3\n");
}

TEST_CASE("slp prints naive log timestamps as written under any --tz", "[cli][e2e][time]")
{
    if (!TzdataStaged())
    {
        SKIP("tzdata is not staged next to slp");
    }
    const std::vector<std::string> firstTimestamp = {
        "--limit", "1", "--columns", "timestamp", "-t", "csv", "--no-header"
    };
    for (const std::string zone : {"UTC", FIXED_ZONE})
    {
        INFO("--tz " << zone);
        const auto result = RunNaiveTimes(zone, firstTimestamp);
        INFO("stderr:\n" << result.err);
        REQUIRE(result.exitCode == 0);
        CHECK(result.out == "2026-03-01 00:00:00.082\n");
    }
}
