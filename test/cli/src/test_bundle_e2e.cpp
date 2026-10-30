#include "slp_process.hpp"

#include <loglib/filter_expression.hpp>
#include <loglib/log_configuration.hpp>

#include <catch2/catch_test_macros.hpp>

#include <test_common/temp_dir.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

using slp::test::FixturesDir;
using slp::test::RunSlp;

namespace
{

constexpr std::string_view FOUR_ROWS =
    "{\"ts\":\"2024-01-01T00:00:00Z\",\"level\":\"info\",\"msg\":\"alpha\",\"service\":\"auth\"}\n"
    "{\"ts\":\"2024-01-01T00:00:01Z\",\"level\":\"error\",\"msg\":\"bravo\",\"service\":\"auth\"}\n"
    "{\"ts\":\"2024-01-01T00:00:02Z\",\"level\":\"warn\",\"msg\":\"charlie\",\"service\":\"db\"}\n"
    "{\"ts\":\"2024-01-01T00:00:03Z\",\"level\":\"error\",\"msg\":\"delta\",\"service\":\"db\"}\n";

[[nodiscard]] std::filesystem::path ScratchRoot()
{
    // One directory per process, because test cases run in parallel processes.
    static const test_common::TempDir ROOT("slp_cli_bundle");
    return ROOT.Path();
}

[[nodiscard]] std::string EmptyTemplates()
{
    return (FixturesDir() / "empty_templates").string();
}

[[nodiscard]] const std::filesystem::path &WriteLog()
{
    static const std::filesystem::path LOG_PATH = [] {
        const auto file =
            ScratchRoot() /
            ("four_rows_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".jsonl");
        std::ofstream output(file, std::ios::binary | std::ios::trunc);
        output << FOUR_ROWS;
        output.close();
        REQUIRE(output.good());
        return file;
    }();
    return LOG_PATH;
}

[[nodiscard]] slp::test::CommandResult RunBundle(std::vector<std::string> extra)
{
    std::vector<std::string> args = {"bundle", "--tz", "UTC", "--template-dir", EmptyTemplates()};
    args.insert(args.end(), extra.begin(), extra.end());
    args.push_back(WriteLog().string());
    return RunSlp(args);
}

[[nodiscard]] slp::test::CommandResult CountRows(
    const std::filesystem::path &bundle, std::vector<std::string> extra = {}
)
{
    std::vector<std::string> args = {"query", "--tz", "UTC", "--template-dir", EmptyTemplates(), "--count"};
    args.insert(args.end(), extra.begin(), extra.end());
    args.push_back(bundle.string());
    return RunSlp(args);
}

/** @brief Reads back the configuration embedded in @p bundle through `query --save-session`. */
[[nodiscard]] loglib::LogConfiguration EmbeddedConfiguration(
    const std::filesystem::path &bundle, const std::string &name
)
{
    const auto sessionPath = ScratchRoot() / name;
    std::filesystem::remove(sessionPath);
    const auto result = RunSlp(
        {"query",
         "--tz",
         "UTC",
         "--template-dir",
         EmptyTemplates(),
         "--count",
         "--allow-empty",
         "--save-session",
         sessionPath.string(),
         bundle.string()}
    );
    INFO(result.err);
    REQUIRE(result.exitCode == 0);
    loglib::LogConfigurationManager manager;
    manager.Load(sessionPath);
    return manager.Configuration();
}

} // namespace

TEST_CASE("slp bundle keeps every row and stores filter, sort, and columns", "[cli][bundle][e2e]")
{
    const auto bundle = ScratchRoot() / "default.slvbundle";
    std::filesystem::remove(bundle);

    const auto result =
        RunBundle({"-o", bundle.string(), "--filter", "level:error", "--sort", "msg:desc", "--columns", "msg,level"});
    INFO(result.err);
    REQUIRE(result.exitCode == 0);
    REQUIRE(std::filesystem::exists(bundle));

    // The stored filter applies on read ...
    CHECK(CountRows(bundle).out == "2\n");
    // ... but the excluded rows are still in the file: a flag replaces the stored filter.
    CHECK(CountRows(bundle, {"--filter", "msg:alpha"}).out == "1\n");
    CHECK(CountRows(bundle, {"--filter", "msg:charlie"}).out == "1\n");

    const loglib::LogConfiguration written = EmbeddedConfiguration(bundle, "default_session.json");
    CHECK_FALSE(written.expression == loglib::FilterExpression{});
    REQUIRE(written.sort.columnIndex >= 0);
    CHECK(written.columns[static_cast<std::size_t>(written.sort.columnIndex)].header == "msg");
    CHECK(written.sort.descending);
    REQUIRE(written.columns.size() >= 2);
    CHECK(written.columns[0].header == "msg");
    CHECK(written.columns[1].header == "level");
    CHECK(written.columns[0].visible);
    CHECK_FALSE(written.columns.back().visible);
}

TEST_CASE("slp bundle --filtered-rows writes only matching rows", "[cli][bundle][e2e]")
{
    const auto bundle = ScratchRoot() / "filtered.slvbundle";
    std::filesystem::remove(bundle);

    const auto result = RunBundle({"-o", bundle.string(), "--filter", "level:error", "--filtered-rows"});
    INFO(result.err);
    REQUIRE(result.exitCode == 0);

    CHECK(CountRows(bundle).out == "2\n");
    const auto excluded = CountRows(bundle, {"--filter", "msg:alpha"});
    CHECK(excluded.out == "0\n");
    CHECK(excluded.exitCode == 1);

    const loglib::LogConfiguration written = EmbeddedConfiguration(bundle, "filtered_session.json");
    CHECK(written.expression == loglib::FilterExpression{});
}

TEST_CASE("slp bundle appends a missing .slvbundle extension once", "[cli][bundle][e2e]")
{
    const auto bare = ScratchRoot() / "bare_name";
    const auto appended = ScratchRoot() / "bare_name.slvbundle";
    const auto upper = ScratchRoot() / "upper.SLVBUNDLE";
    std::filesystem::remove(appended);
    std::filesystem::remove(upper);

    REQUIRE(RunBundle({"-o", bare.string()}).exitCode == 0);
    CHECK(std::filesystem::exists(appended));
    CHECK_FALSE(std::filesystem::exists(bare));

    REQUIRE(RunBundle({"-o", upper.string()}).exitCode == 0);
    CHECK(std::filesystem::exists(upper));
    CHECK_FALSE(std::filesystem::exists(ScratchRoot() / "upper.SLVBUNDLE.slvbundle"));
}

TEST_CASE("slp bundle requires --output and rejects stdout", "[cli][bundle][e2e]")
{
    const auto missing = RunBundle({});
    CHECK(missing.exitCode == 2);
    CHECK(missing.err.contains("--output"));

    const auto dash = RunBundle({"-o", "-"});
    CHECK(dash.exitCode == 2);
}

TEST_CASE("slp bundle seeds the embedded configuration from --config", "[cli][bundle][e2e]")
{
    const auto firstBundle = ScratchRoot() / "seed_source.slvbundle";
    REQUIRE(RunBundle({"-o", firstBundle.string(), "--filter", "level:error", "--sort", "msg:desc"}).exitCode == 0);
    const loglib::LogConfiguration seed = EmbeddedConfiguration(firstBundle, "seed_session.json");

    const auto configPath = ScratchRoot() / "seed_config.json";
    loglib::LogConfigurationManager::Save(seed, configPath, loglib::SaveScope::Full);

    const auto seeded = ScratchRoot() / "seeded.slvbundle";
    std::filesystem::remove(seeded);
    const auto result = RunBundle({"-o", seeded.string(), "--config", configPath.string()});
    INFO(result.err);
    REQUIRE(result.exitCode == 0);

    CHECK(CountRows(seeded).out == "2\n");
    const loglib::LogConfiguration written = EmbeddedConfiguration(seeded, "seeded_session.json");
    CHECK(written.expression == seed.expression);
    CHECK(written.sort.descending);

    const auto missing = RunBundle({"-o", seeded.string(), "--config", (ScratchRoot() / "nope.json").string()});
    CHECK(missing.exitCode == 3);
}

TEST_CASE("slp bundle accepts compression level and worker threads", "[cli][bundle][e2e]")
{
    const auto bundle = ScratchRoot() / "tuned.slvbundle";
    std::filesystem::remove(bundle);
    const auto result = RunBundle({"-o", bundle.string(), "--compression-level", "1", "--bundle-threads", "2"});
    INFO(result.err);
    REQUIRE(result.exitCode == 0);
    CHECK(CountRows(bundle).out == "4\n");
}

TEST_CASE("slp bundle reports a write failure as exit 5 and leaves no file", "[cli][bundle][e2e]")
{
    const auto blocker = ScratchRoot() / "bundle_blocker";
    std::filesystem::remove_all(blocker);
    std::filesystem::create_directories(blocker);
    const auto destination = blocker / "missing_dir" / "out.slvbundle";

    const auto result = RunBundle({"-o", destination.string()});
    INFO(result.err);
    CHECK(result.exitCode == 5);
    CHECK(result.err.starts_with("slp bundle: "));
    CHECK_FALSE(std::filesystem::exists(destination));
    CHECK(std::filesystem::is_empty(blocker));
}

TEST_CASE("slp bundle writes no progress line when stderr is not a terminal", "[cli][bundle][e2e]")
{
    const auto bundle = ScratchRoot() / "quiet.slvbundle";
    const auto result = RunBundle({"-o", bundle.string()});
    REQUIRE(result.exitCode == 0);
    CHECK(result.err.empty());
    CHECK(result.out.empty());
}
