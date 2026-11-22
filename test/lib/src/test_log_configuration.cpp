#include "common.hpp"

#include <loglib/config/column_layout.hpp>
#include <loglib/config/filter_expression.hpp>
#include <loglib/config/log_configuration.hpp>
#include <loglib/core/error.hpp>
#include <loglib/core/path_key.hpp>
#include <loglib/query/filter_compile.hpp>

#include <test_common/temp_dir.hpp>

#include <catch2/catch_all.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

using namespace loglib;

namespace
{

[[nodiscard]] FilterExpression LeavesAsExpression(std::vector<FilterCondition> leaves)
{
    FilterExpression::And node;
    for (auto &rule : leaves)
    {
        FilterExpression child;
        child.node = FilterExpression::Leaf{std::move(rule)};
        node.children.push_back(std::move(child));
    }
    FilterExpression expression;
    expression.node = std::move(node);
    return expression;
}

[[nodiscard]] std::string ReadAll(const std::string &path)
{
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

/** A configuration that sets every part: columns of each kind, a full view and a source. */
[[nodiscard]] LogConfiguration MakeFullConfiguration()
{
    LogConfiguration configuration;

    Column detected;
    detected.header = "Message";
    detected.keys = {"msg", "message"};
    detected.printFormat = "{}";
    configuration.columns.push_back(detected);

    Column time;
    time.header = "When";
    time.keys = {"ts"};
    time.printFormat = "%F %T";
    time.type = ColumnType::Time;
    time.parseFormats = {"%FT%T", "%F %T"};
    time.autoDetect = false;
    configuration.columns.push_back(time);

    Column level;
    level.header = "Severity";
    level.keys = {"level"};
    level.printFormat = "{}";
    level.type = ColumnType::Level;
    level.levelMapping = {{"fatal!", "Fatal"}, {"warning", "Warn"}};
    level.autoDetect = false;
    configuration.columns.push_back(level);

    Column hidden;
    hidden.header = "Latency";
    hidden.keys = {"latency"};
    hidden.printFormat = "{:.2f}";
    hidden.type = ColumnType::Floating;
    hidden.visible = false;
    hidden.autoDetect = false;
    configuration.columns.push_back(hidden);

    FilterCondition leaf;
    leaf.type = FilterCondition::Type::String;
    leaf.columnKeys = {"msg", "message"};
    leaf.matchType = FilterCondition::Match::Contains;
    leaf.filterString = "boot";
    configuration.view.expression = LeavesAsExpression({leaf});
    configuration.view.sort = Sort{.columnKeys = {"ts"}, .direction = loglib::SortDirection::Descending};
    configuration.view.highlightRules.push_back(
        HighlightRule{.name = "errors", .condition = {.columnKeys = {"level"}, .filterString = std::string{"error"}}}
    );
    configuration.view.anchors.push_back(
        AnchorEntry{.pathKey = "c:/logs/app.json", .lineId = 7, .colorIndex = 2, .note = "start"}
    );

    SourceDescriptor source;
    source.kind = SourceDescriptor::Kind::File;
    source.format.format = loglib::LogFormat::Json;
    source.locators = {{.path = "C:/Logs/App.json", .key = "c:/logs/app.json"}};
    configuration.source = source;
    return configuration;
}

void CheckSameColumns(const std::vector<Column> &actual, const std::vector<Column> &expected)
{
    REQUIRE(actual.size() == expected.size());
    for (size_t i = 0; i < actual.size(); ++i)
    {
        INFO("column " << i);
        CHECK(actual[i].header == expected[i].header);
        CHECK(actual[i].keys == expected[i].keys);
        CHECK(actual[i].printFormat == expected[i].printFormat);
        CHECK(actual[i].type == expected[i].type);
        CHECK(actual[i].parseFormats == expected[i].parseFormats);
        CHECK(actual[i].visible == expected[i].visible);
        CHECK(actual[i].levelMapping == expected[i].levelMapping);
        CHECK(actual[i].autoDetect == expected[i].autoDetect);
    }
}

/** Parses text that must be rejected and returns the message. */
[[nodiscard]] std::string ErrorOf(std::string_view text, std::string_view origin = "test.json")
{
    try
    {
        static_cast<void>(ParseConfiguration(text, origin));
    }
    catch (const FormatError &error)
    {
        return error.what();
    }
    FAIL("the configuration was accepted");
    return {};
}

constexpr std::string_view MINIMAL = R"({"version":1,"columns":[]})";

} // namespace

TEST_CASE("A full configuration round-trips through a file", "[log_configuration]")
{
    const LogConfiguration original = MakeFullConfiguration();
    const TestLogConfiguration file("test_log_configuration_full.json");
    SaveConfiguration(original, file.GetFilePath(), SaveScope::Full);

    const LogConfiguration loaded = LoadConfiguration(file.GetFilePath());
    CheckSameColumns(loaded.columns, original.columns);
    CHECK(loaded.view.expression == original.view.expression);
    CHECK(loaded.view.sort.columnKeys == original.view.sort.columnKeys);
    CHECK(loaded.view.sort.direction == loglib::SortDirection::Descending);
    CHECK(loaded.view.highlightRules == original.view.highlightRules);
    CHECK(loaded.view.anchors == original.view.anchors);
    REQUIRE(loaded.source.has_value());
    CHECK(loaded.source->kind == SourceDescriptor::Kind::File);
    CHECK(loaded.source->locators == original.source->locators);
}

TEST_CASE("Saving a loaded configuration again gives the same bytes", "[log_configuration]")
{
    const TestLogConfiguration first("test_log_configuration_first.json");
    const TestLogConfiguration second("test_log_configuration_second.json");
    SaveConfiguration(MakeFullConfiguration(), first.GetFilePath(), SaveScope::Full);
    SaveConfiguration(LoadConfiguration(first.GetFilePath()), second.GetFilePath(), SaveScope::Full);
    CHECK(ReadAll(first.GetFilePath()) == ReadAll(second.GetFilePath()));

    const TestLogConfiguration columnsFirst("test_log_configuration_columns_first.json");
    const TestLogConfiguration columnsSecond("test_log_configuration_columns_second.json");
    SaveConfiguration(MakeFullConfiguration(), columnsFirst.GetFilePath(), SaveScope::Columns);
    SaveConfiguration(LoadConfiguration(columnsFirst.GetFilePath()), columnsSecond.GetFilePath(), SaveScope::Columns);
    CHECK(ReadAll(columnsFirst.GetFilePath()) == ReadAll(columnsSecond.GetFilePath()));
}

TEST_CASE("Every configuration fixture loads, saves and loads again to equal values", "[log_configuration]")
{
    const std::filesystem::path directory = LOGLIB_TEST_CONFIGURATION_FIXTURES;
    size_t count = 0;
    for (const auto &entry : std::filesystem::directory_iterator(directory))
    {
        if (entry.path().extension() != ".json" || entry.path().filename().string().starts_with("invalid_"))
        {
            continue;
        }
        INFO(entry.path().string());
        ++count;
        const LogConfiguration loaded = LoadConfiguration(entry.path());
        const std::string once = SerializeConfiguration(loaded);
        const LogConfiguration reloaded = ParseConfiguration(once, "saved");
        CheckSameColumns(reloaded.columns, loaded.columns);
        CHECK(reloaded.view.expression == loaded.view.expression);
        CHECK(reloaded.view.sort.columnKeys == loaded.view.sort.columnKeys);
        CHECK(reloaded.view.highlightRules == loaded.view.highlightRules);
        CHECK(reloaded.view.anchors == loaded.view.anchors);
        CHECK(reloaded.source.has_value() == loaded.source.has_value());
        CHECK(SerializeConfiguration(reloaded) == once);
    }
    CHECK(count >= 3);
}

TEST_CASE("A Columns save writes the columns and nothing else", "[log_configuration][save_scope]")
{
    const TestLogConfiguration file("test_log_configuration_scope.json");
    SaveConfiguration(MakeFullConfiguration(), file.GetFilePath(), SaveScope::Columns);

    const std::string raw = ReadAll(file.GetFilePath());
    CHECK(raw.contains("\"version\""));
    CHECK(raw.contains("\"columns\""));
    CHECK_FALSE(raw.contains("\"view\""));
    CHECK_FALSE(raw.contains("\"expression\""));
    CHECK_FALSE(raw.contains("\"highlightRules\""));
    CHECK_FALSE(raw.contains("\"source\""));

    const LogConfiguration loaded = LoadConfiguration(file.GetFilePath());
    CHECK(loaded.columns.size() == 4);
    CHECK(AcceptsEverything(loaded.view.expression));
    CHECK(loaded.view.sort.columnKeys.empty());
    CHECK(loaded.view.anchors.empty());
    CHECK_FALSE(loaded.source.has_value());
}

TEST_CASE("A Full save writes the documented keys", "[log_configuration][wire_format]")
{
    const std::string raw = SerializeConfiguration(MakeFullConfiguration());
    for (const char *key : {
             "\"version\": 1", "\"columns\"",     "\"view\"",         "\"source\"",    "\"header\"",
             "\"keys\"",       "\"printFormat\"", "\"parseFormats\"", "\"visible\"",   "\"levelMapping\"",
             "\"expression\"", "\"sort\"",        "\"columnKeys\"",   "\"direction\"", "\"highlightRules\"",
             "\"anchors\"",    "\"locators\"",    "\"path\"",         "\"key\"",
         })
    {
        INFO(key);
        CHECK(raw.contains(key));
    }
    CHECK_FALSE(raw.contains("autoDetect"));
}

TEST_CASE("A compact serialization is one line and reads back", "[log_configuration]")
{
    const std::string compact =
        SerializeConfiguration(MakeFullConfiguration(), SaveScope::Full, ConfigurationLayout::Compact);
    CHECK_FALSE(compact.contains('\n'));
    CheckSameColumns(ParseConfiguration(compact).columns, MakeFullConfiguration().columns);
}

TEST_CASE("A column is saved with its type only when the user chose it", "[log_configuration][auto_detect]")
{
    const LogConfiguration configuration = MakeFullConfiguration();
    const std::string raw = SerializeConfiguration(configuration);

    // The first column was detected, so its object has no type. The others carry theirs.
    const size_t first = raw.find("\"header\": \"Message\"");
    const size_t second = raw.find("\"header\": \"When\"");
    REQUIRE(first != std::string::npos);
    REQUIRE(second != std::string::npos);
    CHECK_FALSE(raw.substr(first, second - first).contains("\"type\""));
    CHECK(raw.contains("\"type\": \"time\""));
    CHECK(raw.contains("\"type\": \"level\""));
    CHECK(raw.contains("\"type\": \"floating\""));

    const LogConfiguration loaded = ParseConfiguration(raw);
    REQUIRE(loaded.columns.size() == 4);
    CHECK(loaded.columns[0].autoDetect);
    CHECK(loaded.columns[0].type == ColumnType::Any);
    for (size_t i = 1; i < loaded.columns.size(); ++i)
    {
        CHECK_FALSE(loaded.columns[i].autoDetect);
        CHECK(loaded.columns[i].type == configuration.columns[i].type);
    }
}

TEST_CASE("A detected type is not saved even when the column holds one", "[log_configuration][auto_detect]")
{
    Column column;
    column.header = "Count";
    column.keys = {"count"};
    column.type = ColumnType::Integer;
    column.autoDetect = true;
    LogConfiguration configuration;
    configuration.columns.push_back(column);

    const std::string raw = SerializeConfiguration(configuration);
    CHECK_FALSE(raw.contains("\"type\""));
    const LogConfiguration loaded = ParseConfiguration(raw);
    CHECK(loaded.columns[0].autoDetect);
    CHECK(loaded.columns[0].type == ColumnType::Any);
}

TEST_CASE("Every ColumnType survives a round trip when chosen", "[log_configuration][type_round_trip]")
{
    LogConfiguration configuration;
    for (const ColumnType type : {
             ColumnType::Any,
             ColumnType::String,
             ColumnType::Boolean,
             ColumnType::Integer,
             ColumnType::Floating,
             ColumnType::Number,
             ColumnType::Time,
             ColumnType::Enumeration,
             ColumnType::Level,
         })
    {
        Column column;
        column.header = "c" + std::to_string(static_cast<int>(type));
        column.keys = {column.header};
        column.type = type;
        column.autoDetect = false;
        configuration.columns.push_back(column);
    }
    const LogConfiguration loaded = ParseConfiguration(SerializeConfiguration(configuration));
    CheckSameColumns(loaded.columns, configuration.columns);
}

TEST_CASE("Missing optional fields take the defaults of their members", "[log_configuration][defaults]")
{
    const LogConfiguration loaded =
        ParseConfiguration(R"({"version":1,"columns":[{"header":"a","keys":["a"]}]})", "defaults.json");
    REQUIRE(loaded.columns.size() == 1);
    const Column defaults;
    CHECK(loaded.columns[0].printFormat == defaults.printFormat);
    CHECK(loaded.columns[0].parseFormats == defaults.parseFormats);
    CHECK(loaded.columns[0].visible == defaults.visible);
    CHECK(loaded.columns[0].levelMapping == defaults.levelMapping);
    CHECK(loaded.columns[0].type == ColumnType::Any);
    CHECK(loaded.columns[0].autoDetect);
    CHECK(AcceptsEverything(loaded.view.expression));
    CHECK(loaded.view.sort.columnKeys.empty());
    CHECK(loaded.view.sort.direction == loglib::SortDirection::Ascending);
    CHECK(loaded.view.highlightRules.empty());
    CHECK(loaded.view.anchors.empty());
    CHECK_FALSE(loaded.source.has_value());
}

TEST_CASE("A view may leave out any of its parts", "[log_configuration][defaults]")
{
    const LogConfiguration loaded = ParseConfiguration(
        R"({"version":1,"columns":[],"view":{"sort":{"columnKeys":["a"],"direction":"descending"}}})", "view.json"
    );
    CHECK(loaded.view.sort.columnKeys == std::vector<std::string>{"a"});
    CHECK(loaded.view.sort.direction == loglib::SortDirection::Descending);
    CHECK(AcceptsEverything(loaded.view.expression));
    CHECK(loaded.view.anchors.empty());
}

TEST_CASE("A sort direction is saved by name", "[log_configuration][view]")
{
    for (const auto &[direction, name] : {
             std::pair{SortDirection::Ascending, std::string_view{"ascending"}},
             std::pair{SortDirection::Descending, std::string_view{"descending"}},
         })
    {
        LogConfiguration configuration;
        configuration.view.sort = Sort{.columnKeys = {"ts"}, .direction = direction};
        const std::string json = SerializeConfiguration(configuration, SaveScope::Full, ConfigurationLayout::Compact);
        CHECK(json.contains(R"("sort":{"columnKeys":["ts"],"direction":")" + std::string(name) + R"("})"));
        CHECK(ParseConfiguration(json).view.sort.direction == direction);
    }
    CHECK(ErrorOf(R"({"version":1,"columns":[],"view":{"sort":{"direction":"sideways"}}})").size() > 0);
}

TEST_CASE("A missing version is a FormatError that names the file", "[log_configuration][version]")
{
    const std::string message = ErrorOf(R"({"columns":[]})", "no-version.json");
    CHECK(message.contains("no-version.json"));
    CHECK(message.contains("version"));
}

TEST_CASE("A version other than 1 is a FormatError that names the file", "[log_configuration][version]")
{
    for (const char *text : {R"({"version":2,"columns":[]})", R"({"version":0,"columns":[]})"})
    {
        const std::string message = ErrorOf(text, "future.json");
        CHECK(message.contains("future.json"));
        CHECK(message.contains("version"));
    }
    CHECK_NOTHROW(ParseConfiguration(MINIMAL));
}

TEST_CASE("An unknown key is a FormatError that names the key and the file", "[log_configuration][strict]")
{
    SECTION("at the top level")
    {
        const std::string message = ErrorOf(R"({"version":1,"columns":[],"futureTopKey":1})", "top.json");
        CHECK(message.contains("futureTopKey"));
        CHECK(message.contains("top.json"));
    }
    SECTION("in a column")
    {
        const std::string message =
            ErrorOf(R"({"version":1,"columns":[{"header":"a","keys":["a"],"futureColumnKey":true}]})", "column.json");
        CHECK(message.contains("futureColumnKey"));
        CHECK(message.contains("column.json"));
    }
    SECTION("in the view")
    {
        const std::string message = ErrorOf(R"({"version":1,"columns":[],"view":{"futureViewKey":[]}})", "view.json");
        CHECK(message.contains("futureViewKey"));
        CHECK(message.contains("view.json"));
    }
    SECTION("in the source")
    {
        const std::string message =
            ErrorOf(R"({"version":1,"columns":[],"source":{"kind":"file","futureSourceKey":3}})", "source.json");
        CHECK(message.contains("futureSourceKey"));
    }
    SECTION("in a sort, an anchor and a highlight rule")
    {
        CHECK(ErrorOf(R"({"version":1,"columns":[],"view":{"sort":{"extraSortKey":1}}})").contains("extraSortKey"));
        CHECK(ErrorOf(R"({"version":1,"columns":[],"view":{"anchors":[{"extraAnchorKey":1}]}})")
                  .contains("extraAnchorKey"));
        CHECK(ErrorOf(R"({"version":1,"columns":[],"view":{"highlightRules":[{"extraRuleKey":1}]}})")
                  .contains("extraRuleKey"));
    }
    SECTION("in the condition and the style of a highlight rule")
    {
        CHECK(ErrorOf(R"({"version":1,"columns":[],"view":{"highlightRules":[{"condition":{"extraConditionKey":1}}]}})")
                  .contains("extraConditionKey"));
        CHECK(ErrorOf(R"({"version":1,"columns":[],"view":{"highlightRules":[{"style":{"extraStyleKey":1}}]}})")
                  .contains("extraStyleKey"));
    }
    SECTION("the old autoDetect key is not known")
    {
        CHECK(ErrorOf(R"({"version":1,"columns":[{"header":"a","keys":["a"],"autoDetect":false}]})")
                  .contains("autoDetect"));
    }
}

TEST_CASE("A column needs a header and keys", "[log_configuration][strict]")
{
    CHECK(ErrorOf(R"({"version":1,"columns":[{"keys":["a"]}]})").contains("header"));
    CHECK(ErrorOf(R"({"version":1,"columns":[{"header":"a"}]})").contains("keys"));
    CHECK(ErrorOf(R"({"version":1})").contains("columns"));
}

TEST_CASE("Text that is not a configuration is a FormatError", "[log_configuration][strict]")
{
    CHECK_THROWS_AS(ParseConfiguration(""), FormatError);
    CHECK_THROWS_AS(ParseConfiguration("{}"), FormatError);
    CHECK_THROWS_AS(ParseConfiguration("not json"), FormatError);
    CHECK_THROWS_AS(ParseConfiguration(R"({"version":1,"columns":[{"header":"a","keys":["a"],"type":)"), FormatError);
    CHECK_THROWS_AS(
        ParseConfiguration(R"({"version":1,"columns":[{"header":"a","keys":["a"],"type":"complex"}]})"), FormatError
    );
}

TEST_CASE("A missing file is an IoError and an empty file a FormatError", "[log_configuration]")
{
    CHECK_THROWS_AS(LoadConfiguration("non_existent_file.json"), IoError);

    const TestLogConfiguration empty("test_log_configuration_empty.json");
    CHECK_THROWS_AS(LoadConfiguration(empty.GetFilePath()), FormatError);
}

TEST_CASE("A file error names the file", "[log_configuration]")
{
    const TestLogConfiguration file("test_log_configuration_named.json");
    {
        std::ofstream stream(file.GetFilePath());
        stream << R"({"version":1,"columns":[],"nope":true})";
    }
    try
    {
        static_cast<void>(LoadConfiguration(file.GetFilePath()));
        FAIL("the file was accepted");
    }
    catch (const FormatError &error)
    {
        CHECK(std::string(error.what()).contains("test_log_configuration_named.json"));
    }
}

TEST_CASE("Moving a column keeps the sort on the same column without any remapping", "[log_configuration][layout]")
{
    ColumnLayout layout;
    layout.SetColumns(MakeFullConfiguration().columns);
    REQUIRE(layout.Columns().size() == 4);

    const LogConfiguration configuration = MakeFullConfiguration();
    const auto &keys = configuration.view.sort.columnKeys;
    CHECK(FindColumnByKeys(keys, layout.Columns()) == 1);
    layout.MoveColumn(1, 3);
    CHECK(FindColumnByKeys(keys, layout.Columns()) == 3);
    layout.MoveColumn(3, 0);
    CHECK(FindColumnByKeys(keys, layout.Columns()) == 0);
    CHECK(layout.Columns()[0].keys == std::vector<std::string>{"ts"});
}

TEST_CASE("Source round-trips every kind and the logfmt format", "[log_configuration][source]")
{
    for (const SourceDescriptor::Kind kind :
         {SourceDescriptor::Kind::File, SourceDescriptor::Kind::NetworkStream, SourceDescriptor::Kind::Stdin})
    {
        LogConfiguration configuration;
        SourceDescriptor source;
        source.kind = kind;
        source.format.format = loglib::LogFormat::Logfmt;
        source.locators = {{.path = "tcp://127.0.0.1:9000", .key = "tcp://127.0.0.1:9000"}};
        source.followRotationSiblings = false;
        configuration.source = source;

        const LogConfiguration loaded = ParseConfiguration(SerializeConfiguration(configuration));
        REQUIRE(loaded.source.has_value());
        CHECK(loaded.source->kind == kind);
        CHECK(loaded.source->format.format == loglib::LogFormat::Logfmt);
        CHECK_FALSE(loaded.source->followRotationSiblings);
        CHECK(loaded.source->locators == source.locators);
    }
}

TEST_CASE("Source round-trips a multi-file descriptor and a regex pattern", "[log_configuration][source]")
{
    LogConfiguration configuration;
    SourceDescriptor source;
    source.kind = SourceDescriptor::Kind::File;
    source.format.format = loglib::LogFormat::Regex;
    source.locators = {
        {.path = "C:/Logs/a.log", .key = "c:/logs/a.log"},
        {.path = "C:/Logs/b.log", .key = "c:/logs/b.log"},
    };
    source.format.regex.pattern = "(?<msg>.*)";
    source.format.regex.continuation = ContinuationMode::Indented;
    configuration.source = source;

    const LogConfiguration loaded = ParseConfiguration(SerializeConfiguration(configuration));
    REQUIRE(loaded.source.has_value());
    CHECK(loaded.source->locators == source.locators);
    CHECK(loaded.source->format.regex.pattern == "(?<msg>.*)");
    CHECK(loaded.source->format.regex.continuation == ContinuationMode::Indented);
}

TEST_CASE("Locators are saved as objects with a path and a key", "[log_configuration][source]")
{
    const LogConfiguration loaded = ParseConfiguration(
        R"({"version":1,"columns":[],"source":{"kind":"file","locators":[{"path":"C:/p1","key":"c:/p1"},{"path":"p2","key":"k2"}]}})"
    );
    REQUIRE(loaded.source.has_value());
    REQUIRE(loaded.source->locators.size() == 2);
    CHECK(loaded.source->locators[0].path == "C:/p1");
    CHECK(loaded.source->locators[0].key == "c:/p1");
    CHECK(loaded.source->locators[1].path == "p2");
    CHECK(loaded.source->locators[1].key == "k2");

    LogConfiguration configuration;
    configuration.source = SourceDescriptor{.locators = {{.path = "C:/p1", .key = "c:/p1"}}};
    CHECK(SerializeConfiguration(configuration, SaveScope::Full, ConfigurationLayout::Compact)
              .contains(R"("locators":[{"path":"C:/p1","key":"c:/p1"}])"));
}

TEST_CASE("A locator without a key does not load and the message names the key", "[log_configuration][source]")
{
    SECTION("the key is missing")
    {
        const std::string message =
            ErrorOf(R"({"version":1,"columns":[],"source":{"locators":[{"path":"a.log"}]}})", "nokey.json");
        CHECK(message.contains("key"));
        CHECK(message.contains("nokey.json"));
    }
    SECTION("the key is empty")
    {
        const std::string message =
            ErrorOf(R"({"version":1,"columns":[],"source":{"locators":[{"path":"a.log","key":""}]}})", "emptykey.json");
        CHECK(message.contains("key"));
        CHECK(message.contains("a.log"));
    }
    SECTION("the path is missing")
    {
        CHECK(ErrorOf(R"({"version":1,"columns":[],"source":{"locators":[{"key":"k"}]}})").contains("path"));
    }
    SECTION("an unknown key in a locator")
    {
        CHECK(ErrorOf(R"({"version":1,"columns":[],"source":{"locators":[{"path":"a","key":"a","extra":1}]}})")
                  .contains("extra"));
    }
}

TEST_CASE("AppendLocator computes the key and ContainsPathKey finds it", "[log_configuration][source]")
{
    SourceDescriptor source;
    AppendLocator(source, "logs/app.log");
    REQUIRE(source.locators.size() == 1);
    CHECK(source.locators[0].path == "logs/app.log");
    CHECK(source.locators[0].key == loglib::MakePathKey("logs/app.log"));
    CHECK(ContainsPathKey(source, loglib::MakePathKey("./logs/../logs/app.log")));
    CHECK_FALSE(ContainsPathKey(source, "other"));
    ClearLocators(source);
    CHECK_FALSE(HasLocators(source));
}

TEST_CASE("An empty list of locators round-trips as an empty array", "[log_configuration][source]")
{
    LogConfiguration configuration;
    configuration.source = SourceDescriptor{};
    const std::string raw = SerializeConfiguration(configuration);
    CHECK(raw.contains("\"locators\": []"));
    const LogConfiguration loaded = ParseConfiguration(raw);
    REQUIRE(loaded.source.has_value());
    CHECK(loaded.source->locators.empty());
}

TEST_CASE("Anchors and highlight rules round-trip in the view", "[log_configuration][view]")
{
    LogConfiguration configuration;
    configuration.view.anchors = {
        AnchorEntry{.pathKey = "c:/logs/b.json", .lineId = 9, .colorIndex = 1, .note = ""},
        AnchorEntry{.pathKey = "c:/logs/a.json", .lineId = 7, .colorIndex = 2, .note = "n"},
    };
    configuration.view.highlightRules = {
        HighlightRule{.name = "first", .condition = {.columnKeys = {"a"}}},
        HighlightRule{.name = "second", .enabled = false, .condition = {.columnKeys = {"b"}}, .style = {.bold = true}},
    };

    const LogConfiguration loaded = ParseConfiguration(SerializeConfiguration(configuration));
    CHECK(loaded.view.anchors == configuration.view.anchors);
    CHECK(loaded.view.highlightRules == configuration.view.highlightRules);
}

TEST_CASE(
    "A highlight rule of every condition type round-trips with a nested condition and style",
    "[log_configuration][view]"
)
{
    LogConfiguration configuration;
    configuration.view.highlightRules = {
        {
            .name = "text",
            .condition =
                {
                    .type = FilterCondition::Type::String,
                    .columnKeys = {"msg"},
                    .matchType = FilterCondition::Match::Wildcard,
                    .filterString = "a*",
                },
            .style = {.foregroundIndex = 1, .backgroundIndex = 2, .bold = true, .italic = true},
        },
        {
            .name = "time",
            .condition =
                {.type = FilterCondition::Type::Time, .columnKeys = {"ts"}, .filterBegin = 10, .filterEnd = 20},
        },
        {
            .name = "level",
            .condition =
                {
                    .type = FilterCondition::Type::Enumeration,
                    .columnKeys = {"level"},
                    .filterValues = {"Error", "Warn"},
                },
        },
        {
            .name = "latency",
            .condition =
                {
                    .type = FilterCondition::Type::Number,
                    .columnKeys = {"latency"},
                    .filterMinValue = 1.5,
                    .filterMaxValue = 9.0,
                },
        },
        {
            .name = "flag",
            .condition = {.type = FilterCondition::Type::Boolean, .columnKeys = {"ok"}, .filterValues = {"false"}},
        },
    };

    const std::string json = SerializeConfiguration(configuration, SaveScope::Full, ConfigurationLayout::Compact);
    CHECK(ParseConfiguration(json).view.highlightRules == configuration.view.highlightRules);
    CHECK(
        json.contains(R"("condition":{"type":"string","columnKeys":["msg"],"matchType":"wildcard","filterString":"a*")")
    );
    CHECK(json.contains(R"("style":{"foregroundIndex":1,"backgroundIndex":2,"bold":true,"italic":true})"));
}

TEST_CASE("An anchor without a note loads with an empty note", "[log_configuration][view]")
{
    const LogConfiguration loaded = ParseConfiguration(
        R"({"version":1,"columns":[],"view":{"anchors":[{"pathKey":"c:/l.json","lineId":7,"colorIndex":2}]}})"
    );
    REQUIRE(loaded.view.anchors.size() == 1);
    CHECK(loaded.view.anchors[0].note.empty());
}

TEST_CASE("A filter of every kind round-trips", "[log_configuration][view]")
{
    FilterCondition number;
    number.type = FilterCondition::Type::Number;
    number.columnKeys = {"latency"};
    number.filterMinValue = 1.5;
    number.filterMaxValue = 9.0;

    FilterCondition boolean;
    boolean.type = FilterCondition::Type::Boolean;
    boolean.columnKeys = {"ok"};
    boolean.filterValues = {"true"};

    FilterCondition enumeration;
    enumeration.type = FilterCondition::Type::Enumeration;
    enumeration.columnKeys = {"level"};
    enumeration.filterValues = {"Warn", "Error"};

    FilterExpression::Not negated;
    negated.child = std::make_unique<FilterExpression>(LeavesAsExpression({boolean}));
    FilterExpression::Or either;
    either.children.push_back(LeavesAsExpression({number}));
    either.children.push_back(LeavesAsExpression({enumeration}));

    LogConfiguration configuration;
    FilterExpression::And root;
    FilterExpression orNode;
    orNode.node = std::move(either);
    FilterExpression notNode;
    notNode.node = std::move(negated);
    root.children.push_back(std::move(orNode));
    root.children.push_back(std::move(notNode));
    configuration.view.expression.node = std::move(root);

    const LogConfiguration loaded = ParseConfiguration(SerializeConfiguration(configuration));
    CHECK(loaded.view.expression == configuration.view.expression);
}

TEST_CASE("SaveConfiguration waits for a briefly busy destination", "[log_configuration]")
{
    const test_common::TempDir directory("save_busy");
    const std::filesystem::path path = directory.Path() / "config.json";
    const LogConfiguration configuration;
    SaveConfiguration(configuration, path, SaveScope::Full);

    auto holder = std::make_unique<std::ifstream>(path);
    REQUIRE(holder->is_open());
    const std::jthread releaser([&holder] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50)); // models a scanner that lets go of the file
        holder.reset();
    });

    CHECK_NOTHROW(SaveConfiguration(configuration, path, SaveScope::Full));
}

#ifdef _WIN32
TEST_CASE("SaveConfiguration gives up when the destination stays busy", "[log_configuration][file_busy_retry]")
{
    const test_common::TempDir dir("config_busy");
    const std::filesystem::path destination = dir.Path() / "busy.json";
    {
        std::ofstream seed(destination);
        seed << "{}";
    }

    // Open the destination with no sharing, so replacing it by a rename fails with an access error until it closes.
    const HANDLE held =
        CreateFileW(destination.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(held != INVALID_HANDLE_VALUE);

    const auto start = std::chrono::steady_clock::now();
    CHECK_THROWS_WITH(
        SaveConfiguration(LogConfiguration{}, destination, SaveScope::Full, std::chrono::milliseconds{60}),
        Catch::Matchers::ContainsSubstring("Failed to rename")
    );
    const auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(elapsed < std::chrono::milliseconds{900});

    // The half-written temporary file does not stay behind.
    std::filesystem::path temporary = destination;
    temporary += ".tmp";
    CHECK_FALSE(std::filesystem::exists(temporary));

    CloseHandle(held);

    // Once the destination is free, the same save succeeds.
    CHECK_NOTHROW(SaveConfiguration(LogConfiguration{}, destination, SaveScope::Full, std::chrono::milliseconds{60}));
}
#endif

TEST_CASE("A format spec round-trips for every format and continuation mode", "[log_configuration][source]")
{
    std::vector<loglib::FormatSpec> specs = {
        {.format = loglib::LogFormat::Json},
        {.format = loglib::LogFormat::Logfmt, .logfmtMultiline = false},
        {.format = loglib::LogFormat::Csv},
    };
    for (const ContinuationMode mode :
         {ContinuationMode::None, ContinuationMode::Indented, ContinuationMode::UntilNextHeader})
    {
        specs.push_back({
            .format = loglib::LogFormat::Regex,
            .regex = {.pattern = "^(?<a>\d+) (?<b>.*)$", .continuation = mode, .headerAnchor = "^\d+"},
        });
    }

    for (const loglib::FormatSpec &spec : specs)
    {
        CAPTURE(static_cast<int>(spec.format), static_cast<int>(spec.regex.continuation));
        LogConfiguration configuration;
        SourceDescriptor source;
        source.format = spec;
        source.locators = {{.path = "tcp://127.0.0.1:9000", .key = "tcp://127.0.0.1:9000"}};
        configuration.source = source;

        const std::string json = SerializeConfiguration(configuration);
        // The regex settings are written for the regex format only.
        CHECK(json.contains("\"regex\"") == (spec.format == loglib::LogFormat::Regex));
        const LogConfiguration loaded = ParseConfiguration(json);
        REQUIRE(loaded.source.has_value());
        CHECK(loaded.source->format == spec);
    }
}

TEST_CASE("An unknown key inside a source format is rejected and named", "[log_configuration][source]")
{
    const auto parse = [](std::string_view format) {
        return ParseConfiguration(
            std::string(R"({"version":1,"columns":[],"source":{"kind":"file","format":)") + std::string(format) + "}}"
        );
    };
    CHECK_NOTHROW(parse(R"j({"type":"regex","regex":{"pattern":"(?<a>.*)"}})j"));
    CHECK_THROWS_WITH(parse(R"({"type":"json","bogusKey":1})"), Catch::Matchers::ContainsSubstring("bogusKey"));
    CHECK_THROWS_WITH(
        parse(R"j({"type":"regex","regex":{"pattern":"(?<a>.*)","bogusRegexKey":1}})j"),
        Catch::Matchers::ContainsSubstring("bogusRegexKey")
    );
}
