#include "common.hpp"

#include <log_generator/formats.hpp>
#include <log_generator/generator.hpp>
#include <log_generator/options.hpp>
#include <log_generator/rotation.hpp>
#include <log_generator/target.hpp>

#include <test_common/temp_dir.hpp>

#include <CLI/CLI.hpp>
#include <catch2/catch_all.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{

using log_generator::GeneratorOptions;
using log_generator::TargetKind;
using test_common::TempDir;

GeneratorOptions Parse(std::vector<std::string> args)
{
    CLI::App app;
    GeneratorOptions options;
    log_generator::AddOptions(app, options);
    std::ranges::reverse(args);
    app.parse(args);
    return options;
}

std::vector<std::string> ReadLines(const std::filesystem::path &path)
{
    std::ifstream in(path, std::ios::binary);
    std::vector<std::string> lines;
    for (std::string line; std::getline(in, line);)
    {
        lines.push_back(line);
    }
    return lines;
}

std::int64_t LineNumberOf(const std::string &jsonLine)
{
    const std::string key = "\"line_number\":";
    const auto at = jsonLine.find(key);
    REQUIRE(at != std::string::npos);
    return std::stoll(jsonLine.substr(at + key.size()));
}

void WriteFile(const std::filesystem::path &path, const std::string &contents)
{
    std::ofstream(path, std::ios::binary) << contents;
}

std::string ReadFile(const std::filesystem::path &path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

} // namespace

TEST_CASE("log_generator ParseTarget recognizes every output kind", "[log_generator_tool]")
{
    CHECK(log_generator::ParseTarget("-").kind == TargetKind::Stdout);

    const auto file = log_generator::ParseTarget("logs/app.jsonl");
    CHECK(file.kind == TargetKind::File);
    CHECK(file.path == std::filesystem::path("logs/app.jsonl"));

    const auto tcp = log_generator::ParseTarget("tcp://127.0.0.1:5141");
    CHECK(tcp.kind == TargetKind::Tcp);
    CHECK(tcp.host == "127.0.0.1");
    CHECK(tcp.port == 5141);

    CHECK(log_generator::ParseTarget("tcp+tls://example.com:6514").kind == TargetKind::TcpTls);

    const auto udp6 = log_generator::ParseTarget("udp://[::1]:5142");
    CHECK(udp6.kind == TargetKind::Udp);
    CHECK(udp6.host == "::1");
    CHECK(udp6.Describe() == "udp://[::1]:5142");
}

TEST_CASE("log_generator ParseTarget rejects malformed outputs", "[log_generator_tool]")
{
    for (const char *text :
         {"",
          "ftp://host:1",
          "tcp://host",
          "tcp://:1",
          "tcp://host:0",
          "tcp://host:70000",
          "tcp://host:12ab",
          "udp://[::1]5142"})
    {
        INFO(text);
        CHECK_THROWS_AS(log_generator::ParseTarget(text), std::invalid_argument);
    }
}

TEST_CASE("log_generator defaults to 10MiB of JSON in generated.jsonl", "[log_generator_tool]")
{
    const GeneratorOptions options = Parse({});
    CHECK(options.format->name == "json");
    CHECK(options.target.kind == TargetKind::File);
    CHECK(options.target.path == std::filesystem::path("generated.jsonl"));
    CHECK(options.limits.bytes == log_generator::DEFAULT_SIZE_LIMIT);
    CHECK_FALSE(options.limits.lines.has_value());
    CHECK_FALSE(options.timing.Simulated());
    CHECK_FALSE(options.rotation.Enabled());
}

TEST_CASE("log_generator default file name follows the format", "[log_generator_tool]")
{
    CHECK(Parse({"-f", "csv"}).target.path == std::filesystem::path("generated.csv"));
    CHECK(Parse({"-f", "SYSLOG"}).target.path == std::filesystem::path("generated_syslog.log"));
}

TEST_CASE("log_generator parses limit and rotation suffixes", "[log_generator_tool]")
{
    const GeneratorOptions options =
        Parse({"-n", "10k", "-s", "2MB", "-d", "3h", "--roll-size", "64KiB", "--roll-lines", "1M"});
    CHECK(options.limits.lines == 10'000U);
    CHECK(options.limits.bytes == 2U * 1024U * 1024U);
    CHECK(options.limits.duration == std::chrono::hours(3));
    CHECK(options.rotation.maxBytes == 64U * 1024U);
    CHECK(options.rotation.maxLines == 1'000'000U);
}

TEST_CASE("log_generator giving any limit drops the default size limit", "[log_generator_tool]")
{
    CHECK_FALSE(Parse({"-n", "5"}).limits.bytes.has_value());
    CHECK_FALSE(Parse({"-d", "1m"}).limits.bytes.has_value());
    const GeneratorOptions forever = Parse({"--forever"});
    CHECK(forever.limits.forever);
    CHECK_FALSE(forever.limits.bytes.has_value());
}

TEST_CASE("log_generator parses timing and choice options", "[log_generator_tool]")
{
    const GeneratorOptions options = Parse(
        {"--start-time",
         "2026-01-01T08:00:00Z",
         "--rate",
         "2.5",
         "--profile",
         "FLAT",
         "--bursts",
         "--roll-strategy",
         "copytruncate",
         "--roll-lines",
         "10"}
    );
    REQUIRE(options.timing.startTime.has_value());
    CHECK(*options.timing.startTime == std::chrono::sys_days(std::chrono::year(2026) / 1 / 1) + std::chrono::hours(8));
    CHECK(options.timing.rate == 2.5);
    CHECK(options.timing.profile == test_common::TrafficProfile::Flat);
    CHECK(options.timing.bursts);
    CHECK(options.rotation.strategy == log_generator::RollStrategy::CopyTruncate);
}

TEST_CASE("log_generator rejects inconsistent options", "[log_generator_tool]")
{
    const std::vector<std::vector<std::string>> invalid = {
        {"-f", "xml"},
        {"-n", "0"},
        {"--rate", "0"},
        {"-d", "5"},
        {"--forever", "-n", "5"},
        {"--start-time", "yesterday"},
        {"--roll-strategy", "move"},
        {"--bursts"},
        {"--profile", "flat"},
        {"-o", "-", "--roll-size", "1M"},
        {"-o", "udp://127.0.0.1:1", "--append"},
        {"-o", "tcp://127.0.0.1:1", "--tls-skip-verify"},
        {"-o", "tcp+tls://127.0.0.1:1", "--tls-cert", "client.pem"},
    };
    for (const auto &args : invalid)
    {
        INFO(args.front() << (args.size() > 1 ? " " + args[1] : std::string()));
        CHECK_THROWS_AS(Parse(args), CLI::ParseError);
    }
}

TEST_CASE("log_generator regex-template formats name shipped templates", "[log_generator_tool]")
{
    for (const auto &spec : log_generator::Formats())
    {
        if (spec.family == log_generator::FormatFamily::RegexTemplate)
        {
            INFO(spec.name);
            CHECK(FindTemplateByName(spec.templateName) != nullptr);
        }
    }
}

TEST_CASE("log_generator RotateFile shifts numbered backups", "[log_generator_tool]")
{
    const TempDir dir("log_generator_rotate");
    const auto path = dir.Path() / "app.log";
    const auto backup = [&path](int n) { return std::filesystem::path(path.string() + "." + std::to_string(n)); };

    SECTION("rename moves the file and drops the oldest backup")
    {
        WriteFile(path, "current");
        WriteFile(backup(1), "older");
        WriteFile(backup(2), "oldest");
        log_generator::RotateFile(path, log_generator::RollStrategy::Rename, 2);
        CHECK_FALSE(std::filesystem::exists(path));
        CHECK(ReadFile(backup(1)) == "current");
        CHECK(ReadFile(backup(2)) == "older");
    }
    SECTION("copytruncate keeps the file for the caller to truncate")
    {
        WriteFile(path, "current");
        log_generator::RotateFile(path, log_generator::RollStrategy::CopyTruncate, 3);
        CHECK(ReadFile(path) == "current");
        CHECK(ReadFile(backup(1)) == "current");
    }
    SECTION("rename without backups deletes the file")
    {
        WriteFile(path, "current");
        log_generator::RotateFile(path, log_generator::RollStrategy::Rename, 0);
        CHECK_FALSE(std::filesystem::exists(path));
        CHECK_FALSE(std::filesystem::exists(backup(1)));
    }
}

TEST_CASE("log_generator Run numbers lines continuously across rotations", "[log_generator_tool]")
{
    const TempDir dir("log_generator_run");
    const auto path = dir.Path() / "out.jsonl";
    log_generator::Run(Parse(
        {"-q", "--start-time", "2026-01-01", "-n", "250", "--roll-lines", "100", "--seed", "3", "-o", path.string()}
    ));

    std::vector<std::string> all;
    for (const char *suffix : {".2", ".1", ""})
    {
        const auto lines = ReadLines(path.string() + suffix);
        all.insert(all.end(), lines.begin(), lines.end());
    }
    REQUIRE(all.size() == 250);
    for (std::size_t i = 0; i < all.size(); ++i)
    {
        REQUIRE(LineNumberOf(all[i]) == static_cast<std::int64_t>(i));
    }
}

TEST_CASE("log_generator Run repeats the CSV header in every rotated file", "[log_generator_tool]")
{
    const TempDir dir("log_generator_csv");
    const auto path = dir.Path() / "out.csv";
    log_generator::Run(Parse(
        {"-q",
         "-f",
         "csv",
         "--start-time",
         "2026-01-01",
         "-n",
         "30",
         "--roll-lines",
         "10",
         "--seed",
         "4",
         "-o",
         path.string()}
    ));

    const auto current = ReadLines(path);
    REQUIRE(current.size() == 11);
    for (const char *suffix : {".1", ".2"})
    {
        const auto lines = ReadLines(path.string() + suffix);
        REQUIRE(lines.size() == 11);
        CHECK(lines.front() == current.front());
    }
}

TEST_CASE("log_generator Run stops a simulated run at its duration", "[log_generator_tool]")
{
    const TempDir dir("log_generator_duration");
    const auto path = dir.Path() / "hour.jsonl";
    log_generator::Run(Parse(
        {"-q",
         "--start-time",
         "2026-01-01",
         "-d",
         "1h",
         "--rate",
         "10",
         "--profile",
         "flat",
         "--seed",
         "5",
         "-o",
         path.string()}
    ));

    const auto lines = ReadLines(path);
    CHECK(static_cast<double>(lines.size()) == Catch::Approx(36'000.0).epsilon(0.05));
    CHECK(lines.back().contains("\"timestamp\":\"2026-01-01T00:"));
}
