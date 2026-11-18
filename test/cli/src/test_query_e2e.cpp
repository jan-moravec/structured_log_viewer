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
    REQUIRE(all.out.contains("\"msg\":\"hello\""));
    REQUIRE(all.out.contains("\"msg\":\"boom\""));
    REQUIRE(!all.err.contains("slp:"));

    const auto filtered =
        RunSlp({"query", "--tz", "UTC", "--template-dir", emptyTemplates, "--filter", "level:error", fixture});
    INFO("stdout:\n" << filtered.out);
    INFO("stderr:\n" << filtered.err);
    REQUIRE(filtered.exitCode == 0);
    REQUIRE(filtered.out.contains("\"msg\":\"boom\""));
    REQUIRE(!filtered.out.contains("\"msg\":\"hello\""));
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
        {"query", "--tz", "UTC", "--template-dir", emptyTemplates, "-v", "-t", "csv", "--columns", "msg", fixture}
    );
    INFO("stdout:\n" << verbose.out);
    INFO("stderr:\n" << verbose.err);
    REQUIRE(verbose.exitCode == 0);
    REQUIRE(verbose.out.contains("hello"));
    REQUIRE(!verbose.out.contains("stage=parse"));
    REQUIRE(verbose.err.contains("stage=parse"));
    REQUIRE(verbose.err.contains("stage=filter"));
}

TEST_CASE("slp query rejects a malformed --filter with a caret", "[cli][e2e][query]")
{
    const auto fixture = (FixturesDir() / "two_lines.jsonl").string();
    const auto emptyTemplates = (FixturesDir() / "empty_templates").string();
    const auto bad =
        RunSlp({"query", "--tz", "UTC", "--template-dir", emptyTemplates, "--filter", "level in [Warn", fixture});
    INFO("stderr:\n" << bad.err);
    REQUIRE(bad.exitCode == 2);
    REQUIRE(bad.err.contains("invalid --filter expression"));
    REQUIRE(bad.err.contains('^'));
}

TEST_CASE("slp query rejects a filter that cannot work", "[cli][e2e][query]")
{
    const auto fixture = (FixturesDir() / "two_lines.jsonl").string();
    const auto emptyTemplates = (FixturesDir() / "empty_templates").string();
    const auto run = [&](const char *filter) {
        return RunSlp({"query", "--tz", "UTC", "--template-dir", emptyTemplates, "--filter", filter, fixture});
    };

    const auto unknownColumn = run("nosuch:x");
    INFO("stderr:" << unknownColumn.err);
    CHECK(unknownColumn.exitCode == 2);
    CHECK(unknownColumn.err.find("unknown column 'nosuch'") != std::string::npos);

    const auto invalidRegex = run("level~/(/");
    INFO("stderr:" << invalidRegex.err);
    CHECK(invalidRegex.exitCode == 2);
    CHECK(invalidRegex.err.find("invalid pattern") != std::string::npos);
}

TEST_CASE("slp query --json-visible-only writes only the chosen columns", "[cli][e2e][query]")
{
    const auto fixture = (FixturesDir() / "two_lines.jsonl").string();
    const auto emptyTemplates = (FixturesDir() / "empty_templates").string();
    const auto result = RunSlp(
        {"query",
         "--tz",
         "UTC",
         "--template-dir",
         emptyTemplates,
         "--json-visible-only",
         "--columns",
         "msg,level",
         "--limit",
         "1",
         fixture}
    );
    INFO("stderr:\n" << result.err);
    REQUIRE(result.exitCode == 0);
    CHECK(result.out == "{\"msg\":\"hello\",\"level\":\"info\"}\n");
}

TEST_CASE("slp query rejects an invalid --regex as a usage error", "[cli][e2e][query]")
{
    const auto fixture = (FixturesDir() / "two_lines.jsonl").string();
    const auto result = RunSlp({"query", "--regex", "^(?<a>unterminated", fixture});
    INFO("stderr:\n" << result.err);
    CHECK(result.exitCode == 2);
    CHECK(result.err.contains("--regex"));
    CHECK(result.out.empty());
}

TEST_CASE("slp query counts the lines that failed to parse", "[cli][e2e][query]")
{
    const auto fixture = (FixturesDir() / "partial_corrupt.jsonl").string();
    const auto emptyTemplates = (FixturesDir() / "empty_templates").string();
    const auto result = RunSlp({"query", "--tz", "UTC", "--template-dir", emptyTemplates, "--count", fixture});
    INFO("stderr:\n" << result.err);
    REQUIRE(result.exitCode == 0);
    CHECK(result.err.contains("1 line could not be parsed"));
}
