#include "slp_process.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using slp::test::FixturesDir;
using slp::test::RequireGolden;
using slp::test::RunSlp;

namespace
{

[[nodiscard]] std::string EmptyTemplates()
{
    return (FixturesDir() / "empty_templates").string();
}

[[nodiscard]] std::string TwoLines()
{
    return (FixturesDir() / "two_lines.jsonl").string();
}

} // namespace

TEST_CASE("golden: stats json by level for two_lines", "[cli][e2e][stats][golden]")
{
    const auto result =
        RunSlp({"stats", "--tz", "UTC", "--template-dir", EmptyTemplates(), "--output-format", "json", TwoLines()});
    INFO("stderr:\n" << result.err);
    REQUIRE(result.exitCode == 0);
    RequireGolden("stats_two_lines_level.json", result.out);
}

TEST_CASE("slp stats text by level lists canonical counts", "[cli][e2e][stats]")
{
    const auto result = RunSlp({"stats", "--tz", "UTC", "--template-dir", EmptyTemplates(), TwoLines()});
    INFO("stdout:\n" << result.out);
    INFO("stderr:\n" << result.err);
    REQUIRE(result.exitCode == 0);
    REQUIRE(result.out.find("source:") != std::string::npos);
    REQUIRE(result.out.find("Info") != std::string::npos);
    REQUIRE(result.out.find("Error") != std::string::npos);
    REQUIRE(result.out.find("total: 2") != std::string::npos);
}

TEST_CASE("slp stats by time reports buckets", "[cli][e2e][stats]")
{
    const auto result = RunSlp(
        {"stats", "--tz", "UTC", "--template-dir", EmptyTemplates(), "--by", "time", "--bucket", "1s", TwoLines()}
    );
    INFO("stdout:\n" << result.out);
    REQUIRE(result.exitCode == 0);
    REQUIRE(result.out.find("bucket:") != std::string::npos);
    REQUIRE(result.out.find("2024-01-01") != std::string::npos);
}

TEST_CASE("slp stats by column reports top values", "[cli][e2e][stats]")
{
    const auto result = RunSlp(
        {"stats",
         "--tz",
         "UTC",
         "--template-dir",
         EmptyTemplates(),
         "--by",
         "column",
         "--column",
         "service",
         TwoLines()}
    );
    INFO("stdout:\n" << result.out);
    REQUIRE(result.exitCode == 0);
    REQUIRE(result.out.find("cardinality: 1") != std::string::npos);
    REQUIRE(result.out.find("auth") != std::string::npos);
}

TEST_CASE("slp stats exits 1 when --filter matches nothing", "[cli][e2e][stats]")
{
    const auto result =
        RunSlp({"stats", "--tz", "UTC", "--template-dir", EmptyTemplates(), "--filter", "msg:missing", TwoLines()});
    REQUIRE(result.exitCode == 1);
    REQUIRE(result.out.empty());
}

TEST_CASE("slp stats exits 0 on an empty source", "[cli][e2e][stats]")
{
    const auto fixture = (FixturesDir() / "unparseable.txt").string();
    const auto result = RunSlp({"stats", "--tz", "UTC", "--template-dir", EmptyTemplates(), fixture});
    INFO("stdout:\n" << result.out);
    REQUIRE(result.exitCode == 0);
    REQUIRE(result.out.find("total: 0") != std::string::npos);
}
