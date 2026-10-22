#include "commands.hpp"
#include "options.hpp"

#include <catch2/catch_all.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace
{

[[nodiscard]] std::expected<slp::Options, slp::UsageError> Parse(const std::vector<std::string> &words)
{
    std::vector<std::string> owned = words;
    std::vector<char *> argv;
    for (std::string &word : owned)
    {
        argv.push_back(word.data());
    }
    return slp::ParseArguments(static_cast<int>(argv.size()), argv.data());
}

class TempDir
{
public:
    TempDir()
        : mPath(std::filesystem::temp_directory_path() / "slp-formats-test")
    {
        std::filesystem::remove_all(mPath);
        std::filesystem::create_directories(mPath);
    }

    ~TempDir() noexcept
    {
        std::error_code error;
        std::filesystem::remove_all(mPath, error);
    }

    TempDir(const TempDir &) = delete;
    TempDir &operator=(const TempDir &) = delete;

    [[nodiscard]] const std::filesystem::path &path() const
    {
        return mPath;
    }

private:
    std::filesystem::path mPath;
};

} // namespace

TEST_CASE("formats lists parsers and built-in slugs", "[cli][formats]")
{
    const auto parsed = Parse({"slp", "formats"});
    REQUIRE(parsed.has_value());
    const slp::CommandOutput rendered = slp::RenderFormats(*parsed);
    CHECK(rendered.exitCode == 0);
    CHECK(rendered.out.find("auto") != std::string::npos);
    CHECK(rendered.out.find("json") != std::string::npos);
    CHECK(rendered.out.find("logfmt") != std::string::npos);
    CHECK(rendered.out.find("csv") != std::string::npos);
    CHECK(rendered.out.find("apache_combined") != std::string::npos);
    CHECK(rendered.err.empty());
}

TEST_CASE("formats json and --show", "[cli][formats]")
{
    const auto asJson = Parse({"slp", "formats", "--output-format", "json"});
    REQUIRE(asJson.has_value());
    const slp::CommandOutput json = slp::RenderFormats(*asJson);
    CHECK(json.exitCode == 0);
    CHECK(json.out.find("\"slug\": \"apache_combined\"") != std::string::npos);
    CHECK(json.out.find("\"id\": \"regex\"") != std::string::npos);

    const auto show = Parse({"slp", "formats", "--show", "syslog_rfc3164"});
    REQUIRE(show.has_value());
    const slp::CommandOutput shown = slp::RenderFormats(*show);
    CHECK(shown.exitCode == 0);
    CHECK(shown.out.find("pattern") != std::string::npos);

    const auto missing = Parse({"slp", "formats", "--show", "no-such-template"});
    REQUIRE(missing.has_value());
    const slp::CommandOutput failed = slp::RenderFormats(*missing);
    CHECK(failed.exitCode == 2);
    CHECK(failed.err.find("no-such-template") != std::string::npos);
    CHECK(failed.out.empty());
}

TEST_CASE("formats --user marks a shadowing template", "[cli][formats]")
{
    TempDir dir;
    {
        std::ofstream out(dir.path() / "mine.json", std::ios::binary);
        out << R"({"name":"Apache/nginx Combined Log Format","pattern":"x","description":"user copy"})";
    }
    const auto parsed =
        Parse({"slp", "--template-dir", dir.path().string(), "formats", "--user", "--output-format", "text"});
    REQUIRE(parsed.has_value());
    const slp::CommandOutput rendered = slp::RenderFormats(*parsed);
    CHECK(rendered.exitCode == 0);
    CHECK(rendered.out.find("mine") != std::string::npos);
    CHECK(rendered.out.find("shadows apache_combined") != std::string::npos);
    CHECK(rendered.out.find("priority 100") != std::string::npos);
}
