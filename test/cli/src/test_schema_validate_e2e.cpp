#include "slp_process.hpp"

#include <test_common/log_format.hpp>
#include <test_common/log_generator.hpp>
#include <test_common/temp_dir.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
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

/// Writes @p lineCount generated records through @p format, each ending in a newline.
[[nodiscard]] std::string WriteGenerated(
    const test_common::LogFormat &format, std::size_t lineCount, const std::string &fileName
)
{
    // One directory per process, because test cases run in parallel processes.
    static const test_common::TempDir ROOT("slp_cli_validate");
    const auto path = ROOT.Path() / fileName;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    REQUIRE(static_cast<bool>(output));
    for (const auto &record : test_common::GenerateRandomLogRecords(lineCount, 0xA11CE))
    {
        output << format.writeLine(record) << '\n';
    }
    REQUIRE(static_cast<bool>(output));
    return path.string();
}

} // namespace

TEST_CASE("slp schema text lists columns for a clean file", "[cli][e2e][schema]")
{
    const auto result = RunSlp({"schema", "--tz", "UTC", "--template-dir", EmptyTemplates(), TwoLines()});
    INFO("stdout:\n" << result.out);
    INFO("stderr:\n" << result.err);
    REQUIRE(result.exitCode == 0);
    REQUIRE(result.out.contains("header"));
    REQUIRE(result.out.contains("ts"));
    REQUIRE(result.out.contains("level"));
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
    REQUIRE(result.out.contains("format: json"));
    REQUIRE(result.out.contains("errors: 0"));
}

TEST_CASE("golden: validate json for two_lines", "[cli][e2e][validate][golden]")
{
    const auto result =
        RunSlp({"validate", "--tz", "UTC", "--template-dir", EmptyTemplates(), "--output-format", "json", TwoLines()});
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
    REQUIRE(result.out.contains("errors: 1"));
    REQUIRE(result.out.contains("Error on line 2:"));

    const auto allowed =
        RunSlp({"validate", "--tz", "UTC", "--template-dir", EmptyTemplates(), "--max-error-rate", "50", fixture});
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
    REQUIRE(result.out.contains("errors:"));
}

TEST_CASE("slp validate counts physical lines without the trailing newline", "[cli][e2e][validate]")
{
    constexpr std::size_t LINE_COUNT = 1000;
    const auto json = WriteGenerated(test_common::JsonLines(), LINE_COUNT, "generated.jsonl");
    const auto syslog = WriteGenerated(test_common::SyslogRfc3164Format(), LINE_COUNT, "generated_syslog.log");

    for (const auto &path : {json, syslog})
    {
        const auto result =
            RunSlp({"validate", "--tz", "UTC", "--template-dir", EmptyTemplates(), "--output-format", "json", path});
        INFO("path: " << path);
        INFO("stdout:\n" << result.out);
        INFO("stderr:\n" << result.err);
        REQUIRE(result.exitCode == 0);
        CHECK(result.out.contains(std::format("\"linesConsumed\": {},", LINE_COUNT)));
    }
}
