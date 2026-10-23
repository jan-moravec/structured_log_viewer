#include "slp_process.hpp"

#include <catch2/catch_test_macros.hpp>

using slp::test::FixturesDir;
using slp::test::RequireGolden;
using slp::test::RunSlp;

TEST_CASE("golden: query jsonl all rows", "[cli][golden][query]")
{
    const auto result = RunSlp(
        {"query",
         "--tz",
         "UTC",
         "--template-dir",
         (FixturesDir() / "empty_templates").string(),
         "-t",
         "jsonl",
         (FixturesDir() / "two_lines.jsonl").string()}
    );
    REQUIRE(result.exitCode == 0);
    RequireGolden("query_two_lines.jsonl", result.out);
}

TEST_CASE("golden: query csv projected columns", "[cli][golden][query]")
{
    const auto result = RunSlp(
        {"query",
         "--tz",
         "UTC",
         "--template-dir",
         (FixturesDir() / "empty_templates").string(),
         "-t",
         "csv",
         "--columns",
         "level,msg",
         (FixturesDir() / "two_lines.jsonl").string()}
    );
    REQUIRE(result.exitCode == 0);
    RequireGolden("query_two_lines_level_msg.csv", result.out);
}

TEST_CASE("golden: query markdown filtered errors", "[cli][golden][query]")
{
    const auto result = RunSlp(
        {"query",
         "--tz",
         "UTC",
         "--template-dir",
         (FixturesDir() / "empty_templates").string(),
         "--filter",
         "level:error",
         "-t",
         "markdown",
         "--columns",
         "level,msg",
         (FixturesDir() / "two_lines.jsonl").string()}
    );
    REQUIRE(result.exitCode == 0);
    RequireGolden("query_two_lines_errors.md", result.out);
}
