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

TEST_CASE("slp schema text lists columns for a clean file", "[cli][e2e][schema]")
{
    const auto result = RunSlp({"schema", "--tz", "UTC", "--template-dir", EmptyTemplates(), TwoLines()});
    INFO("stdout:\n" << result.out);
    INFO("stderr:\n" << result.err);
    REQUIRE(result.exitCode == 0);
    REQUIRE(result.out.find("header") != std::string::npos);
    REQUIRE(result.out.find("ts") != std::string::npos);
    REQUIRE(result.out.find("level") != std::string::npos);
}

TEST_CASE("golden: schema json for two_lines", "[cli][e2e][schema][golden]")
{
    const auto result = RunSlp(
        {"schema",
         "--tz",
         "UTC",
         "--template-dir",
         EmptyTemplates(),
         "--output-format",
         "json",
         "--sample",
         "2",
         TwoLines()}
    );
    INFO("stderr:\n" << result.err);
    REQUIRE(result.exitCode == 0);
    RequireGolden("schema_two_lines.json", result.out);
}

TEST_CASE("slp validate reports clean parse health", "[cli][e2e][validate]")
{
    const auto result = RunSlp({"validate", "--tz", "UTC", "--template-dir", EmptyTemplates(), TwoLines()});
    INFO("stdout:\n" << result.out);
    INFO("stderr:\n" << result.err);
    REQUIRE(result.exitCode == 0);
    REQUIRE(result.out.find("format: json") != std::string::npos);
    REQUIRE(result.out.find("errors: 0") != std::string::npos);
}

TEST_CASE("golden: validate json for two_lines", "[cli][e2e][validate][golden]")
{
    const auto result = RunSlp(
        {"validate", "--tz", "UTC", "--template-dir", EmptyTemplates(), "--output-format", "json", TwoLines()}
    );
    INFO("stderr:\n" << result.err);
    REQUIRE(result.exitCode == 0);
    RequireGolden("validate_two_lines.json", result.out);
}

TEST_CASE("slp validate exits 4 on partial corruption", "[cli][e2e][validate]")
{
    const auto fixture = (FixturesDir() / "partial_corrupt.jsonl").string();
    const auto result = RunSlp({"validate", "--tz", "UTC", "--template-dir", EmptyTemplates(), fixture});
    INFO("stdout:\n" << result.out);
    INFO("stderr:\n" << result.err);
    REQUIRE(result.exitCode == 4);
    REQUIRE(result.out.find("errors: 1") != std::string::npos);
    REQUIRE(result.out.find("Error on line 2:") != std::string::npos);

    const auto allowed = RunSlp(
        {"validate",
         "--tz",
         "UTC",
         "--template-dir",
         EmptyTemplates(),
         "--max-error-rate",
         "50",
         fixture}
    );
    INFO("stdout:\n" << allowed.out);
    REQUIRE(allowed.exitCode == 0);
}

TEST_CASE("slp schema exits 3 on a wholly unparseable file", "[cli][e2e][schema]")
{
    const auto fixture = (FixturesDir() / "unparseable.txt").string();
    const auto result = RunSlp({"schema", "--tz", "UTC", "--template-dir", EmptyTemplates(), fixture});
    INFO("stdout:\n" << result.out);
    INFO("stderr:\n" << result.err);
    REQUIRE(result.exitCode == 3);
}

TEST_CASE("slp validate exits 4 on a wholly unparseable file", "[cli][e2e][validate]")
{
    const auto fixture = (FixturesDir() / "unparseable.txt").string();
    const auto result = RunSlp({"validate", "--tz", "UTC", "--template-dir", EmptyTemplates(), fixture});
    INFO("stdout:\n" << result.out);
    INFO("stderr:\n" << result.err);
    REQUIRE(result.exitCode == 4);
    REQUIRE(result.out.find("errors:") != std::string::npos);
}
