#include "captured_console.hpp"
#include "commands.hpp"
#include "options.hpp"

#include <test_common/temp_dir.hpp>

#include <catch2/catch_all.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <variant>

namespace
{

[[nodiscard]] std::expected<slp::Options, slp::UsageError> Parse(const std::vector<std::string> &words)
{
    std::vector<std::string> owned = words;
    std::vector<char *> argv;
    argv.reserve(owned.size());
    for (std::string &word : owned)
    {
        argv.push_back(word.data());
    }
    return slp::ParseArguments(static_cast<int>(argv.size()), argv.data());
}

[[nodiscard]] slp::test::Captured RenderFormats(const slp::Options &options)
{
    return slp::test::Capture(options, [&options](slp::Console &console) {
        return slp::RunFormats(std::get<slp::FormatsOptions>(options.command), options.global, console);
    });
}

} // namespace

TEST_CASE("formats lists parsers and built-in slugs", "[cli][formats]")
{
    const auto parsed = Parse({"slp", "formats"});
    REQUIRE(parsed.has_value());
    const slp::test::Captured rendered = RenderFormats(*parsed);
    CHECK(rendered.exitCode == 0);
    CHECK(rendered.out.contains("auto"));
    CHECK(rendered.out.contains("json"));
    CHECK(rendered.out.contains("logfmt"));
    CHECK(rendered.out.contains("csv"));
    CHECK(rendered.out.contains("apache_combined"));
    CHECK(rendered.err.empty());
}

TEST_CASE("formats json and --show", "[cli][formats]")
{
    const auto asJson = Parse({"slp", "formats", "--output-format", "json"});
    REQUIRE(asJson.has_value());
    const slp::test::Captured json = RenderFormats(*asJson);
    CHECK(json.exitCode == 0);
    CHECK(json.out.contains("\"slug\": \"apache_combined\""));
    CHECK(json.out.contains("\"id\": \"regex\""));

    const auto show = Parse({"slp", "formats", "--show", "syslog_rfc3164"});
    REQUIRE(show.has_value());
    const slp::test::Captured shown = RenderFormats(*show);
    CHECK(shown.exitCode == 0);
    CHECK(shown.out.contains("pattern"));

    const auto missing = Parse({"slp", "formats", "--show", "no-such-template"});
    REQUIRE(missing.has_value());
    const slp::test::Captured failed = RenderFormats(*missing);
    CHECK(failed.exitCode == 2);
    CHECK(failed.err.contains("no-such-template"));
    CHECK(failed.out.empty());
}

TEST_CASE("formats --user marks a shadowing template", "[cli][formats]")
{
    const test_common::TempDir dir("formats");
    {
        std::ofstream out(dir.Path() / "mine.json", std::ios::binary);
        out << R"({"name":"Apache/nginx Combined Log Format","pattern":"x","description":"user copy"})";
    }
    const auto parsed =
        Parse({"slp", "--template-dir", dir.Path().string(), "formats", "--user", "--output-format", "text"});
    REQUIRE(parsed.has_value());
    const slp::test::Captured rendered = RenderFormats(*parsed);
    CHECK(rendered.exitCode == 0);
    CHECK(rendered.out.contains("mine"));
    CHECK(rendered.out.contains("shadows apache_combined"));
    CHECK(rendered.out.contains("priority 100"));
}
