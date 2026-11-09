#include "slp_process.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using slp::test::FixturesDir;
using slp::test::RunSlp;
using slp::test::RunSlpWithStdin;
using slp::test::ScratchRoot;

namespace
{

constexpr int OUTPUT_ERROR = 5;

[[nodiscard]] std::vector<std::string> QueryArgs(std::vector<std::string> extra)
{
    std::vector<std::string> args = {
        "query", "--tz", "UTC", "--template-dir", (FixturesDir() / "empty_templates").string()
    };
    args.insert(args.end(), extra.begin(), extra.end());
    args.push_back((FixturesDir() / "two_lines.jsonl").string());
    return args;
}

[[nodiscard]] std::filesystem::path FreshDirectory(const std::string &name)
{
    const auto directory = ScratchRoot() / name;
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    return directory;
}

} // namespace

TEST_CASE(
    "slp query reports an output that cannot be written as exit 5 and leaves no file", "[cli][query][e2e][failure]"
)
{
    const auto directory = FreshDirectory("query_output_failure");
    const auto destination = directory / "missing_dir" / "out.jsonl";

    const auto result = RunSlp(QueryArgs({"--output", destination.string()}));
    INFO(result.err);
    CHECK(result.exitCode == OUTPUT_ERROR);
    CHECK(result.err.starts_with("slp query: "));
    CHECK(result.out.empty());
    CHECK_FALSE(std::filesystem::exists(destination));
    CHECK(std::filesystem::is_empty(directory));
}

TEST_CASE("slp query reports an output that is a directory as exit 5", "[cli][query][e2e][failure]")
{
    const auto directory = FreshDirectory("query_output_directory");
    const auto destination = directory / "taken";
    std::filesystem::create_directories(destination);

    const auto result = RunSlp(QueryArgs({"--output", destination.string(), "--force"}));
    INFO(result.err);
    CHECK(result.exitCode == OUTPUT_ERROR);
    CHECK(result.err.starts_with("slp query: "));
    CHECK(std::filesystem::is_directory(destination));
}

TEST_CASE("slp query refuses to replace an existing output without --force", "[cli][query][e2e][failure]")
{
    const auto directory = FreshDirectory("query_output_kept");
    const auto destination = directory / "existing.jsonl";
    {
        std::ofstream out(destination, std::ios::binary);
        out << "previous\n";
    }

    const auto refused = RunSlp(QueryArgs({"--output", destination.string()}));
    INFO(refused.err);
    CHECK(refused.exitCode == OUTPUT_ERROR);
    CHECK(refused.err.starts_with("slp query: "));

    std::ifstream in(destination, std::ios::binary);
    const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(content == "previous\n");
}

TEST_CASE("slp tail reports an output file that cannot be opened as exit 5", "[cli][tail][e2e][failure]")
{
    const auto directory = FreshDirectory("tail_output_failure");
    const auto destination = directory / "missing_dir" / "tail.jsonl";

    const auto result = RunSlpWithStdin(
        {"tail",
         "--tz",
         "UTC",
         "--template-dir",
         (FixturesDir() / "empty_templates").string(),
         "--output",
         destination.string(),
         "-"},
        "{\"ts\":\"2024-01-01T00:00:00Z\",\"msg\":\"alpha\"}\n"
    );
    INFO(result.err);
    CHECK(result.exitCode == OUTPUT_ERROR);
    CHECK(result.err.starts_with("slp tail: "));
    CHECK(result.err.find("could not open output") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(destination));
}
