#include "slp_process.hpp"

#include "load_table.hpp"
#include "options.hpp"
#include "session_config.hpp"

#include <loglib/filter_expression.hpp>
#include <loglib/leaf_rule_compile.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_filter.hpp>
#include <loglib/log_table.hpp>
#include <loglib/parse_file.hpp>
#include <loglib/query_parser.hpp>
#include <loglib/session_bundle.hpp>

#include <catch2/catch_test_macros.hpp>

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
    const auto root = std::filesystem::temp_directory_path() / "slp_cli_session_config";
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    return root;
}

[[nodiscard]] std::string EmptyTemplates()
{
    return (FixturesDir() / "empty_templates").string();
}

/** @brief Writes the four-row log once per process; parallel ctest runs get distinct files. */
[[nodiscard]] const std::filesystem::path &WriteLog()
{
    static const std::filesystem::path path = [] {
        const auto file =
            ScratchRoot() /
            ("four_rows_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".jsonl");
        std::ofstream output(file, std::ios::binary | std::ios::trunc);
        output << FOUR_ROWS;
        output.close();
        REQUIRE(output.good());
        return file;
    }();
    return path;
}

/** @brief Configuration detected from the four-row log; the base for authored configs. */
[[nodiscard]] loglib::LogConfiguration DetectedConfiguration()
{
    loglib::ParseResult parsed = loglib::ParseFile(WriteLog());
    REQUIRE(parsed.errors.empty());
    loglib::LogConfigurationManager manager;
    manager.Update(parsed.data);
    return manager.Configuration();
}

[[nodiscard]] loglib::FilterExpression Query(std::string_view text)
{
    auto parsed = loglib::ParseQuery(text);
    REQUIRE(parsed.has_value());
    return std::move(*parsed);
}

[[nodiscard]] int ColumnIndex(const std::vector<loglib::Column> &columns, std::string_view header)
{
    for (std::size_t i = 0; i < columns.size(); ++i)
    {
        if (columns[i].header == header)
        {
            return static_cast<int>(i);
        }
    }
    FAIL("missing column " << header);
    return -1;
}

[[nodiscard]] std::filesystem::path WriteConfig(
    const std::string &name, const loglib::LogConfiguration &configuration, loglib::SaveScope scope
)
{
    const auto path = ScratchRoot() / name;
    loglib::LogConfigurationManager::Save(configuration, path, scope);
    return path;
}

/** @brief Loads the four-row log under @p flags and resolves the session without running a command. */
struct Resolved
{
    slp::LoadedTable loaded;
    slp::EffectiveSession session;
    std::vector<std::size_t> rows;
};

[[nodiscard]] Resolved Resolve(std::vector<std::string> flags)
{
    std::vector<std::string> words = {"slp", "query", "--tz", "UTC", "--template-dir", EmptyTemplates()};
    words.insert(words.end(), flags.begin(), flags.end());
    words.push_back(WriteLog().string());
    std::vector<char *> argv;
    for (std::string &word : words)
    {
        argv.push_back(word.data());
    }
    auto options = slp::ParseArguments(static_cast<int>(argv.size()), argv.data());
    REQUIRE(options.has_value());

    slp::RuntimeContext runtime;
    auto [loaded, side] = slp::LoadTable(*options, runtime);
    INFO(side.err);
    REQUIRE(side.exitCode == 0);
    auto session = slp::ApplySessionConfiguration(*options, loaded.table, runtime, "query");
    if (!session)
    {
        FAIL(session.error().err);
    }

    const auto compiled = loglib::CompileExpression(
        session->expression, loaded.table.Configuration().Configuration().columns, &loaded.table
    );
    auto rows = loglib::FilterAcceptedRows(loaded.table, compiled);
    return Resolved{std::move(loaded), std::move(*session), std::move(rows)};
}

} // namespace

TEST_CASE("session precedence: configuration expression applies without row flags", "[cli][session]")
{
    loglib::LogConfiguration configuration = DetectedConfiguration();
    configuration.expression = Query("level:error");
    const auto path = WriteConfig("expr.json", configuration, loglib::SaveScope::Full);

    const Resolved resolved = Resolve({"--config", path.string()});
    CHECK(resolved.rows == std::vector<std::size_t>{1, 3});
}

TEST_CASE("session precedence: row flags replace the configuration expression", "[cli][session]")
{
    loglib::LogConfiguration configuration = DetectedConfiguration();
    configuration.expression = Query("level:error");
    const auto path = WriteConfig("expr_override.json", configuration, loglib::SaveScope::Full);

    const Resolved resolved = Resolve({"--config", path.string(), "--filter", "msg:alpha"});
    CHECK(resolved.rows == std::vector<std::size_t>{0});
}

TEST_CASE("session precedence: --invert alone negates the configuration expression", "[cli][session]")
{
    loglib::LogConfiguration configuration = DetectedConfiguration();
    configuration.expression = Query("level:error");
    const auto path = WriteConfig("expr_invert.json", configuration, loglib::SaveScope::Full);

    const Resolved resolved = Resolve({"--config", path.string(), "--invert"});
    CHECK(resolved.rows == std::vector<std::size_t>{0, 2});
}

TEST_CASE("session precedence: --sort replaces the configuration sort", "[cli][session]")
{
    loglib::LogConfiguration configuration = DetectedConfiguration();
    const int msg = ColumnIndex(configuration.columns, "msg");
    configuration.sort.columnIndex = msg;
    configuration.sort.descending = true;
    const auto path = WriteConfig("sort.json", configuration, loglib::SaveScope::Full);

    const Resolved fromConfig = Resolve({"--config", path.string()});
    REQUIRE(fromConfig.session.sort.has_value());
    CHECK(fromConfig.session.sort->columnIndex == static_cast<std::size_t>(msg));
    CHECK(fromConfig.session.sort->direction == loglib::SortDirection::Descending);

    const Resolved overridden = Resolve({"--config", path.string(), "--sort", "level:asc"});
    REQUIRE(overridden.session.sort.has_value());
    CHECK(
        overridden.session.sort->columnIndex ==
        static_cast<std::size_t>(ColumnIndex(overridden.loaded.table.Configuration().Configuration().columns, "level"))
    );
    CHECK(overridden.session.sort->direction == loglib::SortDirection::Ascending);
    CHECK_FALSE(overridden.session.configuration.sort.descending);
}

TEST_CASE("session precedence: column flags replace configuration visibility", "[cli][session]")
{
    loglib::LogConfiguration configuration = DetectedConfiguration();
    for (loglib::Column &column : configuration.columns)
    {
        column.visible = column.header != "service";
    }
    const auto path = WriteConfig("columns.json", configuration, loglib::SaveScope::Full);

    const Resolved fromConfig = Resolve({"--config", path.string()});
    CHECK(fromConfig.session.visibleColumns.size() == configuration.columns.size() - 1);

    const Resolved all = Resolve({"--config", path.string(), "--all-columns"});
    CHECK(all.session.visibleColumns.size() == configuration.columns.size());
    for (const loglib::Column &column : all.session.configuration.columns)
    {
        CHECK(column.visible);
    }

    const Resolved picked = Resolve({"--config", path.string(), "--columns", "msg,level"});
    REQUIRE(picked.session.visibleColumns.size() == 2);
    const auto &written = picked.session.configuration.columns;
    CHECK(written[0].header == "msg");
    CHECK(written[1].header == "level");
    CHECK(written[0].visible);
    CHECK(written[1].visible);
    for (std::size_t i = 2; i < written.size(); ++i)
    {
        CHECK_FALSE(written[i].visible);
    }
}

TEST_CASE("session precedence: --config discards the embedded source", "[cli][session]")
{
    loglib::LogConfiguration configuration = DetectedConfiguration();
    loglib::Source source;
    loglib::AppendLocator(source, "C:/elsewhere/other.jsonl", "c:/elsewhere/other.jsonl");
    configuration.source = std::move(source);
    const auto path = WriteConfig("source.json", configuration, loglib::SaveScope::Full);

    const Resolved resolved = Resolve({"--config", path.string()});
    CHECK_FALSE(resolved.session.configuration.source.has_value());
    CHECK(resolved.loaded.table.RowCount() == 4);
}

TEST_CASE("slp query --config failures map to exit 3", "[cli][session][e2e]")
{
    const auto missing = (ScratchRoot() / "missing_config.json").string();
    const auto result =
        RunSlp({"query", "--tz", "UTC", "--template-dir", EmptyTemplates(), "--config", missing, WriteLog().string()});
    INFO("stderr:\n" << result.err);
    CHECK(result.exitCode == 3);
    CHECK(result.err.find("missing_config.json") != std::string::npos);

    const auto malformed = ScratchRoot() / "malformed.json";
    {
        std::ofstream output(malformed, std::ios::binary | std::ios::trunc);
        output << "{ not json";
    }
    const auto parse = RunSlp(
        {"query",
         "--tz",
         "UTC",
         "--template-dir",
         EmptyTemplates(),
         "--config",
         malformed.string(),
         WriteLog().string()}
    );
    INFO("stderr:\n" << parse.err);
    CHECK(parse.exitCode == 3);
    CHECK(parse.err.starts_with("slp: "));
}

TEST_CASE("slp query --config precedence over a bundle and under flags", "[cli][session][e2e]")
{
    loglib::LogConfiguration bundled = DetectedConfiguration();
    bundled.expression = Query("level:error");
    const auto bundlePath = ScratchRoot() / "precedence.slvbundle";
    {
        loglib::ParseResult parsed = loglib::ParseFile(WriteLog());
        REQUIRE(parsed.errors.empty());
        loglib::LogConfigurationManager manager;
        manager.Update(parsed.data);
        loglib::LogTable table{std::move(parsed.data), std::move(manager)};
        loglib::WriteSessionBundle(table, bundled, bundlePath);
    }

    loglib::LogConfiguration overlay = DetectedConfiguration();
    overlay.expression = Query("level:info");
    const auto configPath = WriteConfig("precedence.json", overlay, loglib::SaveScope::Full);

    const std::vector<std::string> base = {"query", "--tz", "UTC", "--template-dir", EmptyTemplates(), "--count"};
    auto run = [&](std::vector<std::string> extra) {
        std::vector<std::string> args = base;
        args.insert(args.end(), extra.begin(), extra.end());
        args.push_back(bundlePath.string());
        return RunSlp(args);
    };

    const auto bundleOnly = run({});
    INFO(bundleOnly.err);
    CHECK(bundleOnly.out == "2\n");

    const auto withConfig = run({"--config", configPath.string()});
    INFO(withConfig.err);
    CHECK(withConfig.out == "1\n");

    const auto withFlag = run({"--config", configPath.string(), "--filter", "level:warn"});
    INFO(withFlag.err);
    CHECK(withFlag.out == "1\n");
}

TEST_CASE("slp query --save-session writes the effective configuration", "[cli][session][e2e]")
{
    const auto log = WriteLog();
    const auto sessionPath = ScratchRoot() / "saved_session.json";
    std::filesystem::remove(sessionPath);

    const std::vector<std::string> args = {
        "query",
        "--tz",
        "UTC",
        "--template-dir",
        EmptyTemplates(),
        "--filter",
        "level:error",
        "--sort",
        "msg:desc",
        "--columns",
        "msg,level",
        "-t",
        "jsonl",
        log.string()
    };
    const auto plain = RunSlp(args);
    REQUIRE(plain.exitCode == 0);

    std::vector<std::string> withSave = args;
    withSave.insert(withSave.begin() + 1, {"--save-session", sessionPath.string()});
    const auto saved = RunSlp(withSave);
    INFO(saved.err);
    CHECK(saved.exitCode == plain.exitCode);
    CHECK(saved.out == plain.out);
    CHECK(saved.err == plain.err);

    loglib::LogConfigurationManager manager;
    manager.Load(sessionPath);
    const loglib::LogConfiguration &written = manager.Configuration();
    REQUIRE(written.sort.columnIndex >= 0);
    CHECK(written.columns[static_cast<std::size_t>(written.sort.columnIndex)].header == "msg");
    CHECK(written.sort.descending);
    REQUIRE(written.columns.size() >= 2);
    CHECK(written.columns[0].header == "msg");
    CHECK(written.columns[1].header == "level");
    CHECK(written.columns[0].visible);
    CHECK_FALSE(written.columns.back().visible);

    REQUIRE(written.source.has_value());
    REQUIRE(written.source->locators.size() == 1);
    const std::filesystem::path locator = written.source->locators[0].displayPath;
    CHECK(locator.is_absolute());
    CHECK(locator.filename() == log.filename());
    CHECK(written.source->format == loglib::Source::Format::Json);

    // The saved session reproduces the filtered row set.
    const auto replay = RunSlp(
        {"query",
         "--tz",
         "UTC",
         "--template-dir",
         EmptyTemplates(),
         "--config",
         sessionPath.string(),
         "-t",
         "jsonl",
         log.string()}
    );
    INFO(replay.err);
    CHECK(replay.exitCode == 0);
    CHECK(replay.out.find("bravo") != std::string::npos);
    CHECK(replay.out.find("alpha") == std::string::npos);
    CHECK(replay.out.find("delta") < replay.out.find("bravo"));
}

TEST_CASE("slp query --save-columns omits session state", "[cli][session][e2e]")
{
    const auto log = WriteLog();
    const auto columnsPath = ScratchRoot() / "saved_columns.json";
    std::filesystem::remove(columnsPath);

    loglib::LogConfiguration configuration = DetectedConfiguration();
    loglib::HighlightRule rule;
    rule.name = "errors";
    rule.columnKeys = {"level"};
    rule.type = loglib::HighlightRule::Type::Enumeration;
    rule.filterValues = {"Error"};
    rule.backgroundIndex = 1;
    configuration.highlightRules.push_back(rule);
    const auto configPath = WriteConfig("with_rules.json", configuration, loglib::SaveScope::Full);

    const auto result = RunSlp(
        {"query",
         "--tz",
         "UTC",
         "--template-dir",
         EmptyTemplates(),
         "--config",
         configPath.string(),
         "--filter",
         "level:error",
         "--save-columns",
         columnsPath.string(),
         "--count",
         log.string()}
    );
    INFO(result.err);
    REQUIRE(result.exitCode == 0);
    CHECK(result.out == "2\n");

    loglib::LogConfigurationManager manager;
    manager.Load(columnsPath);
    const loglib::LogConfiguration &written = manager.Configuration();
    CHECK(written.columns.size() == configuration.columns.size());
    REQUIRE(written.highlightRules.size() == 1);
    CHECK(written.highlightRules[0].name == "errors");
    CHECK(written.expression == loglib::FilterExpression{});
    CHECK(written.sort.columnIndex == -1);
    CHECK_FALSE(written.source.has_value());
}

TEST_CASE("slp query save flags report write failure as exit 5", "[cli][session][e2e]")
{
    const auto blocker = ScratchRoot() / "blocker";
    std::filesystem::remove_all(blocker);
    std::filesystem::create_directories(blocker);

    const auto result = RunSlp(
        {"query",
         "--tz",
         "UTC",
         "--template-dir",
         EmptyTemplates(),
         "--save-session",
         (blocker / "missing_dir" / "session.json").string(),
         "--count",
         WriteLog().string()}
    );
    INFO(result.err);
    CHECK(result.exitCode == 5);
    CHECK(result.err.find("session.json") != std::string::npos);
}

TEST_CASE("slp query --save-session records the compressed primary path only", "[cli][session][e2e]")
{
    const auto root = ScratchRoot() / "rotation";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    {
        std::ofstream primary(root / "app.log", std::ios::binary);
        primary << FOUR_ROWS;
        std::ofstream sibling(root / "app.log.1", std::ios::binary);
        sibling << FOUR_ROWS;
    }
    const auto sessionPath = root / "session.json";

    const auto result = RunSlp(
        {"query",
         "--tz",
         "UTC",
         "--template-dir",
         EmptyTemplates(),
         "--count",
         "--save-session",
         sessionPath.string(),
         (root / "app.log").string()}
    );
    INFO(result.err);
    REQUIRE(result.exitCode == 0);
    CHECK(result.out == "8\n");

    loglib::LogConfigurationManager manager;
    manager.Load(sessionPath);
    REQUIRE(manager.Configuration().source.has_value());
    const auto &locators = manager.Configuration().source->locators;
    REQUIRE(locators.size() == 1);
    CHECK(std::filesystem::path(locators[0].displayPath).filename() == "app.log");
    CHECK(manager.Configuration().source->followRotationSiblings);
}

TEST_CASE("slp query --save-session omits stdin from source", "[cli][session][e2e]")
{
    const auto sessionPath = ScratchRoot() / "stdin_session.json";
    std::filesystem::remove(sessionPath);
    const auto result = slp::test::RunSlpWithStdin(
        {"query",
         "--tz",
         "UTC",
         "--template-dir",
         EmptyTemplates(),
         "--count",
         "--save-session",
         sessionPath.string(),
         "-"},
        FOUR_ROWS
    );
    INFO(result.err);
    REQUIRE(result.exitCode == 0);
    CHECK(result.out == "4\n");

    loglib::LogConfigurationManager manager;
    manager.Load(sessionPath);
    CHECK_FALSE(manager.Configuration().source.has_value());
}

TEST_CASE("slp stats honors a configuration expression", "[cli][session][e2e]")
{
    loglib::LogConfiguration configuration = DetectedConfiguration();
    configuration.expression = Query("level:error");
    // `stats` has no `--config` flag; the bundle feeds it the configuration.
    const auto bundlePath = ScratchRoot() / "stats.slvbundle";
    {
        loglib::ParseResult parsed = loglib::ParseFile(WriteLog());
        loglib::LogConfigurationManager manager;
        manager.Update(parsed.data);
        loglib::LogTable table{std::move(parsed.data), std::move(manager)};
        loglib::WriteSessionBundle(table, configuration, bundlePath);
    }

    const auto result = RunSlp(
        {"stats", "--tz", "UTC", "--template-dir", EmptyTemplates(), "--output-format", "json", bundlePath.string()}
    );
    INFO(result.err);
    REQUIRE(result.exitCode == 0);
    CHECK(result.out.find("\"total\": 2") != std::string::npos);
}
