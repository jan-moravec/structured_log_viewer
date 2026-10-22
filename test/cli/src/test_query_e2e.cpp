#include "slp_process.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using slp::test::FixturesDir;
using slp::test::RunSlp;

TEST_CASE("slp query emits jsonl rows and respects --filter", "[cli][e2e][query]")
{
    const auto fixture = (FixturesDir() / "two_lines.jsonl").string();
    const auto emptyTemplates = (FixturesDir() / "empty_templates").string();
    const auto all = RunSlp({"query", "--tz", "UTC", "--template-dir", emptyTemplates, fixture});
    INFO("stdout:\n" << all.out);
    INFO("stderr:\n" << all.err);
    REQUIRE(all.exitCode == 0);
    REQUIRE(all.out.find("\"msg\":\"hello\"") != std::string::npos);
    REQUIRE(all.out.find("\"msg\":\"boom\"") != std::string::npos);
    REQUIRE(all.err.find("slp:") == std::string::npos);

    const auto filtered =
        RunSlp({"query", "--tz", "UTC", "--template-dir", emptyTemplates, "--filter", "level:error", fixture});
    INFO("stdout:\n" << filtered.out);
    INFO("stderr:\n" << filtered.err);
    REQUIRE(filtered.exitCode == 0);
    REQUIRE(filtered.out.find("\"msg\":\"boom\"") != std::string::npos);
    REQUIRE(filtered.out.find("\"msg\":\"hello\"") == std::string::npos);
}

TEST_CASE("slp query --count and empty results", "[cli][e2e][query]")
{
    const auto fixture = (FixturesDir() / "two_lines.jsonl").string();
    const auto emptyTemplates = (FixturesDir() / "empty_templates").string();
    const auto counted = RunSlp({"query", "--tz", "UTC", "--template-dir", emptyTemplates, "--count", fixture});
    INFO("stdout:\n" << counted.out);
    INFO("stderr:\n" << counted.err);
    REQUIRE(counted.exitCode == 0);
    REQUIRE(counted.out == "2\n");

    const auto empty =
        RunSlp({"query", "--tz", "UTC", "--template-dir", emptyTemplates, "--filter", "msg:missing", fixture});
    REQUIRE(empty.exitCode == 1);
    REQUIRE(empty.out.empty());

    const auto allowed = RunSlp(
        {"query", "--tz", "UTC", "--template-dir", emptyTemplates, "--filter", "msg:missing", "--allow-empty", fixture}
    );
    REQUIRE(allowed.exitCode == 0);
}

TEST_CASE("slp query verbose keeps row bytes on stdout", "[cli][e2e][query]")
{
    const auto fixture = (FixturesDir() / "two_lines.jsonl").string();
    const auto emptyTemplates = (FixturesDir() / "empty_templates").string();
    const auto verbose = RunSlp(
        {"query",
         "--tz",
         "UTC",
         "--template-dir",
         emptyTemplates,
         "-v",
         "-t",
         "csv",
         "--columns",
         "msg",
         fixture}
    );
    INFO("stdout:\n" << verbose.out);
    INFO("stderr:\n" << verbose.err);
    REQUIRE(verbose.exitCode == 0);
    REQUIRE(verbose.out.find("hello") != std::string::npos);
    REQUIRE(verbose.out.find("stage=parse") == std::string::npos);
    REQUIRE(verbose.err.find("stage=parse") != std::string::npos);
    REQUIRE(verbose.err.find("stage=filter") != std::string::npos);
}

TEST_CASE("slp query rejects a malformed --filter with a caret", "[cli][e2e][query]")
{
    const auto fixture = (FixturesDir() / "two_lines.jsonl").string();
    const auto emptyTemplates = (FixturesDir() / "empty_templates").string();
    const auto bad =
        RunSlp({"query", "--tz", "UTC", "--template-dir", emptyTemplates, "--filter", "level in [Warn", fixture});
    INFO("stderr:\n" << bad.err);
    REQUIRE(bad.exitCode == 2);
    REQUIRE(bad.err.find("invalid --filter expression") != std::string::npos);
    REQUIRE(bad.err.find('^') != std::string::npos);
}
