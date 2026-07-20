#include "common.hpp"

#include <loglib/internal/log_configuration_glaze_meta.hpp>
#include <loglib/key_index.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_data.hpp>
#include <loglib/log_level.hpp>
#include <loglib/log_line.hpp>

#include <catch2/catch_all.hpp>
#include <glaze/glaze.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <utility>
#include <vector>

using namespace loglib;

TEST_CASE("Save and load empty configuration", "[LogConfigurationManager]")
{
    const TestLogConfiguration testConfiguration;

    {
        const LogConfigurationManager manager;
        manager.Save(testConfiguration.GetFilePath());
    }

    // Load configuration from the file
    {
        LogConfigurationManager manager;
        manager.Load(testConfiguration.GetFilePath());

        // Verify loaded configuration is empty
        CHECK(manager.Configuration().columns.empty());
        CHECK(manager.Configuration().filters.empty());
    }
}

TEST_CASE("Handle missing file", "[LogConfigurationManager]")
{
    LogConfigurationManager manager;
    static const std::filesystem::path INVALID_PATH = "non_existent_file.json";

    CHECK_THROWS_AS(manager.Load(INVALID_PATH), std::runtime_error);
}

TEST_CASE("Handle empty file", "[LogConfigurationManager]")
{
    const TestLogConfiguration testConfiguration;
    LogConfigurationManager manager;

    CHECK_THROWS_AS(manager.Load(testConfiguration.GetFilePath()), std::runtime_error);
}

TEST_CASE("Update with empty LogData should not modify configuration", "[LogConfigurationManager]")
{
    const TestLogConfiguration testLogConfiguration;

    LogConfiguration logConfiguration;
    const LogConfiguration::Column defaultColumn = {
        .header = "test", .keys = {"test"}, .printFormat = "{}", .type = LogConfiguration::Type::Any, .parseFormats = {}
    };
    logConfiguration.columns.push_back(defaultColumn);
    testLogConfiguration.Write(logConfiguration);

    LogConfigurationManager manager;
    manager.Load(testLogConfiguration.GetFilePath());

    const size_t initialColumnCount = manager.Configuration().columns.size();

    const LogData emptyLogData;
    manager.Update(emptyLogData);

    CHECK(manager.Configuration().columns.size() == initialColumnCount);
    CHECK(manager.Configuration().columns[0].header == defaultColumn.header);
    CHECK(manager.Configuration().columns[0].keys == defaultColumn.keys);
    CHECK(manager.Configuration().columns[0].parseFormats == defaultColumn.parseFormats);
    CHECK(manager.Configuration().columns[0].printFormat == defaultColumn.printFormat);
    CHECK(manager.Configuration().columns[0].type == defaultColumn.type);
}

TEST_CASE(
    "AppendKeys recognises common timestamp aliases as Type::Time and ignores bare 't'",
    "[LogConfigurationManager][timestamp_keys][regression]"
)
{
    // Regression: bare `t` used to be auto-promoted to a date column.
    // The well-known aliases (`@timestamp`, `datetime`, `created_at`,
    // `ts`) now go through the same promotion path.
    LogConfigurationManager manager;
    manager.AppendKeys({"timestamp", "time", "ts", "@timestamp", "datetime", "created_at", "t", "tag"});

    const auto &columns = manager.Configuration().columns;
    REQUIRE(columns.size() == 8);

    auto findColumn = [&columns](std::string_view header) {
        return std::ranges::find_if(columns, [header](const auto &c) { return c.header == header; });
    };

    auto checkType = [&](std::string_view header, LogConfiguration::Type expected) {
        const auto it = findColumn(header);
        REQUIRE(it != columns.end());
        CHECK(it->type == expected);
    };

    checkType("timestamp", LogConfiguration::Type::Time);
    checkType("time", LogConfiguration::Type::Time);
    checkType("ts", LogConfiguration::Type::Time);
    checkType("@timestamp", LogConfiguration::Type::Time);
    checkType("datetime", LogConfiguration::Type::Time);
    checkType("created_at", LogConfiguration::Type::Time);

    checkType("t", LogConfiguration::Type::Any);
    checkType("tag", LogConfiguration::Type::Any);
}

TEST_CASE("Update with mixed keys organizes timestamp first", "[LogConfigurationManager]")
{
    const TestLogConfiguration testLogConfiguration;

    LogConfiguration logConfiguration;
    logConfiguration.columns.push_back(
        {.header = "regular",
         .keys = {"regular"},
         .printFormat = "{}",
         .type = LogConfiguration::Type::Any,
         .parseFormats = {}}
    );
    testLogConfiguration.Write(logConfiguration);

    LogConfigurationManager manager;
    manager.Load(testLogConfiguration.GetFilePath());

    const TestLogFile testLogFile;
    auto source = testLogFile.CreateFileLineSource();
    KeyIndex testKeys;
    std::vector<LogLine> testLines;
    testLines.emplace_back(LogMap{{"regular", std::string("value")}}, testKeys, *source, 0);
    testLines.emplace_back(LogMap{{"newKey", std::string("test")}}, testKeys, *source, 0);
    testLines.emplace_back(LogMap{{"timestamp", std::string("2023-01-01T12:00:00Z")}}, testKeys, *source, 0);

    const LogData logData(std::move(source), std::move(testLines), std::move(testKeys));

    manager.Update(logData);

    REQUIRE(manager.Configuration().columns.size() == 3);

    CHECK(manager.Configuration().columns[0].header == "timestamp");
    CHECK(manager.Configuration().columns[0].type == LogConfiguration::Type::Time);

    CHECK(manager.Configuration().columns[1].header == "regular");
    CHECK(manager.Configuration().columns[2].header == "newKey");

    CHECK(manager.Configuration().columns[0].printFormat == "%F %H:%M:%S");
    REQUIRE(manager.Configuration().columns[0].parseFormats.size() == 4);
    CHECK(manager.Configuration().columns[0].parseFormats[0] == "%FT%T%Ez");
}

// `IsKeyInAnyColumn` cache-invalidation tests: every mutating path
// (`Load`, `Update`, `AppendKeys`) must invalidate the cache.

TEST_CASE(
    "Cache: AppendKeys after a query sees the freshly appended key", "[LogConfigurationManager][cache_invalidation]"
)
{
    LogConfigurationManager manager;

    manager.AppendKeys({"alpha"});
    REQUIRE(manager.Configuration().columns.size() == 1);
    CHECK(manager.Configuration().columns[0].header == "alpha");

    // Duplicate "alpha" inside the call must not double-add.
    manager.AppendKeys({"alpha", "beta", "alpha"});
    REQUIRE(manager.Configuration().columns.size() == 2);
    CHECK(manager.Configuration().columns[0].header == "alpha");
    CHECK(manager.Configuration().columns[1].header == "beta");
}

TEST_CASE(
    "Cache: Update after AppendKeys auto-promotes timestamps with the cache fresh",
    "[LogConfigurationManager][cache_invalidation]"
)
{
    LogConfigurationManager manager;

    manager.AppendKeys({"regular"});
    REQUIRE(manager.Configuration().columns.size() == 1);

    // Update must skip "regular" and add "timestamp" as Type::Time at index 0.
    const TestLogFile testLogFile;
    auto source = testLogFile.CreateFileLineSource();
    KeyIndex testKeys;
    std::vector<LogLine> testLines;
    testLines.emplace_back(LogMap{{"regular", std::string("value")}}, testKeys, *source, 0);
    testLines.emplace_back(LogMap{{"timestamp", std::string("2023-01-01T12:00:00Z")}}, testKeys, *source, 0);
    const LogData logData(std::move(source), std::move(testLines), std::move(testKeys));

    manager.Update(logData);

    REQUIRE(manager.Configuration().columns.size() == 2);
    CHECK(manager.Configuration().columns[0].header == "timestamp");
    CHECK(manager.Configuration().columns[0].type == LogConfiguration::Type::Time);
    CHECK(manager.Configuration().columns[1].header == "regular");
}

TEST_CASE(
    "Cache: Load wholesale-replaces the cache against the freshly loaded columns",
    "[LogConfigurationManager][cache_invalidation]"
)
{
    const TestLogConfiguration firstConfigOnDisk("test_config_first.json");
    {
        LogConfiguration logConfiguration;
        logConfiguration.columns.push_back(
            {.header = "loaded_key",
             .keys = {"loaded_key"},
             .printFormat = "{}",
             .type = LogConfiguration::Type::Any,
             .parseFormats = {}}
        );
        firstConfigOnDisk.Write(logConfiguration);
    }
    const TestLogConfiguration secondConfigOnDisk("test_config_second.json");
    {
        LogConfiguration logConfiguration;
        logConfiguration.columns.push_back(
            {.header = "other_key",
             .keys = {"other_key"},
             .printFormat = "{}",
             .type = LogConfiguration::Type::Any,
             .parseFormats = {}}
        );
        secondConfigOnDisk.Write(logConfiguration);
    }

    LogConfigurationManager manager;

    // Prime the cache with a key the on-disk config does not have; Load
    // must replace the cache so the next AppendKeys re-adds it.
    manager.AppendKeys({"stale_key"});
    REQUIRE(manager.Configuration().columns.size() == 1);
    CHECK(manager.Configuration().columns[0].header == "stale_key");

    manager.Load(firstConfigOnDisk.GetFilePath());
    REQUIRE(manager.Configuration().columns.size() == 1);
    CHECK(manager.Configuration().columns[0].header == "loaded_key");

    manager.AppendKeys({"stale_key", "loaded_key"});
    REQUIRE(manager.Configuration().columns.size() == 2);
    CHECK(manager.Configuration().columns[0].header == "loaded_key");
    CHECK(manager.Configuration().columns[1].header == "stale_key");

    manager.Load(secondConfigOnDisk.GetFilePath());
    REQUIRE(manager.Configuration().columns.size() == 1);
    CHECK(manager.Configuration().columns[0].header == "other_key");

    manager.AppendKeys({"loaded_key", "stale_key", "other_key"});
    REQUIRE(manager.Configuration().columns.size() == 3);
    CHECK(manager.Configuration().columns[0].header == "other_key");
    CHECK(manager.Configuration().columns[1].header == "loaded_key");
    CHECK(manager.Configuration().columns[2].header == "stale_key");
}

TEST_CASE(
    "Cache: parity with the pre-cache O(M*K) walk on the existing fixture",
    "[LogConfigurationManager][cache_invalidation]"
)
{
    // Regression: the cached path matches the old O(M*K) walk on the
    // "timestamp first" fixture.
    const TestLogConfiguration testLogConfiguration;
    LogConfiguration logConfiguration;
    logConfiguration.columns.push_back(
        {.header = "regular",
         .keys = {"regular"},
         .printFormat = "{}",
         .type = LogConfiguration::Type::Any,
         .parseFormats = {}}
    );
    testLogConfiguration.Write(logConfiguration);

    LogConfigurationManager manager;
    manager.Load(testLogConfiguration.GetFilePath());

    const TestLogFile testLogFile;
    auto source = testLogFile.CreateFileLineSource();
    KeyIndex testKeys;
    std::vector<LogLine> testLines;
    testLines.emplace_back(LogMap{{"regular", std::string("value")}}, testKeys, *source, 0);
    testLines.emplace_back(LogMap{{"newKey", std::string("test")}}, testKeys, *source, 0);
    testLines.emplace_back(LogMap{{"timestamp", std::string("2023-01-01T12:00:00Z")}}, testKeys, *source, 0);
    const LogData logData(std::move(source), std::move(testLines), std::move(testKeys));

    manager.Update(logData);

    REQUIRE(manager.Configuration().columns.size() == 3);
    CHECK(manager.Configuration().columns[0].header == "timestamp");
    CHECK(manager.Configuration().columns[1].header == "regular");
    CHECK(manager.Configuration().columns[2].header == "newKey");
}

TEST_CASE(
    "Cache: a column whose `keys` differs from `header` is matched by key",
    "[LogConfigurationManager][cache_invalidation]"
)
{
    // Cache keys on `column.keys`, not `column.header`: AppendKeys must
    // skip keys already listed under any column's `keys`.
    const TestLogConfiguration testLogConfiguration;
    LogConfiguration logConfiguration;
    logConfiguration.columns.push_back(
        {.header = "display",
         .keys = {"raw_key", "alias"},
         .printFormat = "{}",
         .type = LogConfiguration::Type::Any,
         .parseFormats = {}}
    );
    testLogConfiguration.Write(logConfiguration);

    LogConfigurationManager manager;
    manager.Load(testLogConfiguration.GetFilePath());

    manager.AppendKeys({"display", "raw_key", "alias", "fresh"});

    // Only "display" and "fresh" are added ("raw_key"/"alias" hit the cache).
    REQUIRE(manager.Configuration().columns.size() == 3);
    CHECK(manager.Configuration().columns[0].header == "display");
    CHECK(manager.Configuration().columns[1].header == "display"); // header reused; keys differ
    CHECK(manager.Configuration().columns[2].header == "fresh");
    CHECK(manager.Configuration().columns[1].keys == std::vector<std::string>{"display"});
    CHECK(manager.Configuration().columns[2].keys == std::vector<std::string>{"fresh"});
}

TEST_CASE("Save and load configuration with Type::Enumeration column", "[log_configuration][enum]")
{
    const TestLogConfiguration testLogConfiguration("test_log_configuration_enum.json");

    LogConfiguration logConfiguration;
    logConfiguration.columns.push_back(
        {.header = "Level",
         .keys = {"level", "severity"},
         .printFormat = "{}",
         .type = LogConfiguration::Type::Enumeration,
         .parseFormats = {}}
    );
    testLogConfiguration.Write(logConfiguration);

    LogConfigurationManager manager;
    manager.Load(testLogConfiguration.GetFilePath());

    REQUIRE(manager.Configuration().columns.size() == 1);
    CHECK(manager.Configuration().columns[0].header == "Level");
    CHECK(manager.Configuration().columns[0].type == LogConfiguration::Type::Enumeration);
    CHECK(manager.Configuration().columns[0].keys == std::vector<std::string>{"level", "severity"});
}

TEST_CASE(
    "Newly-discovered keys default to Type::Any with autoDetect=true so the auto-detector scans them",
    "[log_configuration][auto_detect_default]"
)
{
    // Provenance is carried by the `(type, autoDetect)` pair: a fresh
    // key starts as `(Type::Any, autoDetect=true)`, i.e. the
    // auto-detector candidate state.

    SECTION("AppendKeys assigns the candidate state to fresh keys")
    {
        LogConfigurationManager manager;
        manager.AppendKeys({"level"});
        REQUIRE(manager.Configuration().columns.size() == 1);
        CHECK(manager.Configuration().columns[0].type == LogConfiguration::Type::Any);
        CHECK(manager.Configuration().columns[0].autoDetect);
    }

    SECTION("Update assigns the candidate state to every freshly-added non-time key")
    {
        const TestLogFile testLogFile;
        auto source = testLogFile.CreateFileLineSource();
        KeyIndex keys;
        std::vector<LogLine> lines;
        lines.emplace_back(LogMap{{"k1", std::string("v1")}, {"k2", std::string("v2")}}, keys, *source, 0);
        const LogData logData(std::move(source), std::move(lines), std::move(keys));

        LogConfigurationManager manager;
        manager.Update(logData);

        REQUIRE(manager.Configuration().columns.size() == 2);
        for (const auto &column : manager.Configuration().columns)
        {
            CHECK(column.type == LogConfiguration::Type::Any);
            CHECK(column.autoDetect);
        }
    }

    SECTION("Loaded columns keep their stored Type and autoDetect flag")
    {
        const TestLogConfiguration testCfg;
        LogConfiguration cfg;
        cfg.columns.push_back(
            {.header = "level",
             .keys = {"level"},
             .printFormat = "{}",
             .type = LogConfiguration::Type::Any,
             .parseFormats = {},
             .visible = true,
             .levelMapping = {},
             .autoDetect = false}
        );
        cfg.columns.push_back(
            {.header = "service",
             .keys = {"service"},
             .printFormat = "{}",
             .type = LogConfiguration::Type::String,
             .parseFormats = {}}
        );
        testCfg.Write(cfg);

        LogConfigurationManager manager;
        manager.Load(testCfg.GetFilePath());

        REQUIRE(manager.Configuration().columns.size() == 2);
        CHECK(manager.Configuration().columns[0].type == LogConfiguration::Type::Any);
        CHECK_FALSE(manager.Configuration().columns[0].autoDetect);
        CHECK(manager.Configuration().columns[1].type == LogConfiguration::Type::String);
    }

    SECTION("Post-Load AppendKeys still assigns the candidate state to genuinely-new keys")
    {
        const TestLogConfiguration testCfg;
        LogConfiguration cfg;
        cfg.columns.push_back(
            {.header = "level",
             .keys = {"level"},
             .printFormat = "{}",
             .type = LogConfiguration::Type::Any,
             .parseFormats = {},
             .visible = true,
             .levelMapping = {},
             .autoDetect = false}
        );
        testCfg.Write(cfg);

        LogConfigurationManager manager;
        manager.Load(testCfg.GetFilePath());

        manager.AppendKeys({"freshly_streamed"});
        REQUIRE(manager.Configuration().columns.size() == 2);
        CHECK(manager.Configuration().columns[0].type == LogConfiguration::Type::Any);
        CHECK_FALSE(manager.Configuration().columns[0].autoDetect);
        CHECK(manager.Configuration().columns[1].type == LogConfiguration::Type::Any);
        CHECK(manager.Configuration().columns[1].autoDetect);
    }
}

TEST_CASE("Round-trip preserves every LogConfiguration::Type variant", "[log_configuration][type_round_trip]")
{
    // C++ enumerators are UpperCamelCase but the Glaze meta keeps the
    // wire format as the original lowerCamelCase strings so existing
    // saved configurations stay loadable. `double` is reserved, so both
    // sides spell it `floating`.
    using Type = LogConfiguration::Type;
    const std::vector<Type> variants = {
        Type::Any,
        Type::String,
        Type::Boolean,
        Type::Integer,
        Type::Floating,
        Type::Number,
        Type::Time,
        Type::Enumeration,
        Type::Level,
    };

    LogConfiguration original;
    for (size_t i = 0; i < variants.size(); ++i)
    {
        original.columns.push_back(
            {.header = "col-" + std::to_string(i),
             .keys = {"col-" + std::to_string(i)},
             .printFormat = "{}",
             .type = variants[i],
             .parseFormats = {}}
        );
    }

    std::string json;
    const auto writeError = glz::write_json(original, json);
    REQUIRE_FALSE(writeError);

    CHECK(json.contains("\"any\""));
    CHECK(json.contains("\"boolean\""));
    CHECK(json.contains("\"integer\""));
    CHECK(json.contains("\"floating\""));
    CHECK_FALSE(json.contains("\"double\""));
    CHECK(json.contains("\"number\""));
    CHECK(json.contains("\"enumeration\""));
    // `Type::Level` shares storage with Enumeration at runtime but
    // has its own wire key so a saved Level column reloads as Level.
    CHECK(json.contains("\"level\""));

    LogConfiguration loaded;
    const auto readError = glz::read_json(loaded, json);
    REQUIRE_FALSE(readError);

    REQUIRE(loaded.columns.size() == variants.size());
    for (size_t i = 0; i < variants.size(); ++i)
    {
        CHECK(loaded.columns[i].type == variants[i]);
    }
}

TEST_CASE(
    "SaveScope::ColumnsOnly omits filters/sort/source so the file is portable across data sources",
    "[log_configuration][save_scope][session]"
)
{
    // A configuration-shape file is the wire-format strict subset of
    // a session-shape file: same JSON schema, just with the
    // session-only fields at their default values.
    LogConfigurationManager manager;
    manager.AppendKeys({"timestamp", "msg"});
    REQUIRE(manager.Configuration().columns.size() == 2);

    // Populate session-only state (filter on row 0, sort by column 1
    // descending, file-source).
    LogConfiguration::LogFilter filter;
    filter.type = LogConfiguration::LogFilter::Type::String;
    filter.row = 0;
    filter.filterString = "boot";
    filter.matchType = LogConfiguration::LogFilter::Match::Contains;
    manager.SetFilters({filter});
    manager.SetSort(LogConfiguration::Sort{.columnIndex = 1, .descending = true});
    manager.SetSource(
        LogConfiguration::Source{.kind = LogConfiguration::Source::Kind::File, .locators = {"C:/logs/app.json"}}
    );

    const TestLogConfiguration columnsOnlyFile;
    manager.Save(columnsOnlyFile.GetFilePath(), SaveScope::ColumnsOnly);

    // The on-disk JSON must not even mention the session-only
    // fields: a transient `LogConfiguration` would still serialise
    // empty `"filters"` / a default `"sort"` because those members
    // are not optional. The `ColumnsOnlyDocument` shim in
    // `log_configuration.cpp` exists exactly to keep the wire output
    // clean for users who inspect saved configs by hand.
    {
        std::ifstream readBack(columnsOnlyFile.GetFilePath());
        REQUIRE(readBack.is_open());
        const std::string raw((std::istreambuf_iterator<char>(readBack)), std::istreambuf_iterator<char>());
        CHECK(raw.contains("\"columns\""));
        CHECK_FALSE(raw.contains("\"filters\""));
        CHECK_FALSE(raw.contains("\"sort\""));
        CHECK_FALSE(raw.contains("\"source\""));
    }

    // Re-load: only columns survive; filters/sort/source are absent
    // and reload at their default (inert) values.
    LogConfigurationManager reloadedFromColumns;
    reloadedFromColumns.Load(columnsOnlyFile.GetFilePath());
    REQUIRE(reloadedFromColumns.Configuration().columns.size() == 2);
    CHECK(reloadedFromColumns.Configuration().filters.empty());
    CHECK(reloadedFromColumns.Configuration().sort.columnIndex == -1);
    CHECK_FALSE(reloadedFromColumns.Configuration().sort.descending);
    CHECK_FALSE(reloadedFromColumns.Configuration().source.has_value());

    // SaveScope::Full writes every field; session-only state
    // round-trips intact.
    const TestLogConfiguration fullFile;
    manager.Save(fullFile.GetFilePath(), SaveScope::Full);

    LogConfigurationManager reloadedFromFull;
    reloadedFromFull.Load(fullFile.GetFilePath());
    REQUIRE(reloadedFromFull.Configuration().columns.size() == 2);
    REQUIRE(reloadedFromFull.Configuration().filters.size() == 1);
    CHECK(reloadedFromFull.Configuration().filters[0].row == 0);
    REQUIRE(reloadedFromFull.Configuration().filters[0].filterString.has_value());
    CHECK(*reloadedFromFull.Configuration().filters[0].filterString == "boot");
    CHECK(reloadedFromFull.Configuration().sort.columnIndex == 1);
    CHECK(reloadedFromFull.Configuration().sort.descending);
    REQUIRE(reloadedFromFull.Configuration().source.has_value());
    CHECK(reloadedFromFull.Configuration().source->kind == LogConfiguration::Source::Kind::File);
    REQUIRE(reloadedFromFull.Configuration().source->locators.size() == 1);
    CHECK(reloadedFromFull.Configuration().source->locators.front() == "C:/logs/app.json");
}

TEST_CASE("LogConfiguration::Source round-trips both Kind variants", "[log_configuration][session][source]")
{
    LogConfiguration original;
    original.source = LogConfiguration::Source{
        .kind = LogConfiguration::Source::Kind::NetworkStream, .locators = {"tcp://127.0.0.1:5170"}
    };

    std::string json;
    const auto writeError = glz::write_json(original, json);
    REQUIRE_FALSE(writeError);
    CHECK(json.contains("\"networkStream\""));

    LogConfiguration loaded;
    const auto readError = glz::read_json(loaded, json);
    REQUIRE_FALSE(readError);

    REQUIRE(loaded.source.has_value());
    CHECK(loaded.source->kind == LogConfiguration::Source::Kind::NetworkStream);
    REQUIRE(loaded.source->locators.size() == 1);
    CHECK(loaded.source->locators.front() == "tcp://127.0.0.1:5170");
}

TEST_CASE("LogConfiguration::Source round-trips a multi-file `File` descriptor", "[log_configuration][session][source]")
{
    LogConfiguration original;
    original.source = LogConfiguration::Source{
        .kind = LogConfiguration::Source::Kind::File,
        .locators = {"C:/logs/first.json", "C:/logs/second.json", "C:/logs/third.json"}
    };

    std::string json;
    const auto writeError = glz::write_json(original, json);
    REQUIRE_FALSE(writeError);

    LogConfiguration loaded;
    const auto readError = glz::read_json(loaded, json);
    REQUIRE_FALSE(readError);

    REQUIRE(loaded.source.has_value());
    CHECK(loaded.source->kind == LogConfiguration::Source::Kind::File);
    REQUIRE(loaded.source->locators.size() == 3);
    CHECK(loaded.source->locators[0] == "C:/logs/first.json");
    CHECK(loaded.source->locators[1] == "C:/logs/second.json");
    CHECK(loaded.source->locators[2] == "C:/logs/third.json");
}

TEST_CASE("LogConfiguration::Source round-trips Format::Logfmt", "[log_configuration][session][source]")
{
    LogConfiguration original;
    original.source = LogConfiguration::Source{
        .kind = LogConfiguration::Source::Kind::File,
        .format = LogConfiguration::Source::Format::Logfmt,
        .locators = {"C:/logs/app.logfmt"}
    };

    std::string json;
    const auto writeError = glz::write_json(original, json);
    REQUIRE_FALSE(writeError);
    CHECK(json.contains("\"logfmt\""));

    LogConfiguration loaded;
    const auto readError = glz::read_json(loaded, json);
    REQUIRE_FALSE(readError);

    REQUIRE(loaded.source.has_value());
    CHECK(loaded.source->kind == LogConfiguration::Source::Kind::File);
    CHECK(loaded.source->format == LogConfiguration::Source::Format::Logfmt);
    REQUIRE(loaded.source->locators.size() == 1);
    CHECK(loaded.source->locators.front() == "C:/logs/app.logfmt");
}

TEST_CASE("Round-trip LogFilter with Type::Enumeration and filterValues", "[log_configuration][enum]")
{
    LogConfiguration original;
    LogConfiguration::LogFilter filter;
    filter.type = LogConfiguration::LogFilter::Type::Enumeration;
    filter.row = 2;
    filter.filterValues = {"info", "warn", "error"};
    original.filters.push_back(filter);

    std::string json;
    const auto writeError = glz::write_json(original, json);
    REQUIRE_FALSE(writeError);

    LogConfiguration loaded;
    const auto readError = glz::read_json(loaded, json);
    REQUIRE_FALSE(readError);

    REQUIRE(loaded.filters.size() == 1);
    CHECK(loaded.filters[0].type == LogConfiguration::LogFilter::Type::Enumeration);
    CHECK(loaded.filters[0].row == 2);
    CHECK(loaded.filters[0].filterValues == std::vector<std::string>{"info", "warn", "error"});
}

TEST_CASE("Round-trip LogFilter with Type::Number and bounded / unbounded range", "[log_configuration][number]")
{
    LogConfiguration original;

    LogConfiguration::LogFilter bounded;
    bounded.type = LogConfiguration::LogFilter::Type::Number;
    bounded.row = 1;
    bounded.filterMinValue = -2.5;
    bounded.filterMaxValue = 17.25;
    original.filters.push_back(bounded);

    LogConfiguration::LogFilter unboundedMin;
    unboundedMin.type = LogConfiguration::LogFilter::Type::Number;
    unboundedMin.row = 2;
    unboundedMin.filterMaxValue = 100.0;
    original.filters.push_back(unboundedMin);

    LogConfiguration::LogFilter unboundedMax;
    unboundedMax.type = LogConfiguration::LogFilter::Type::Number;
    unboundedMax.row = 3;
    unboundedMax.filterMinValue = 0.0;
    original.filters.push_back(unboundedMax);

    std::string json;
    const auto writeError = glz::write_json(original, json);
    REQUIRE_FALSE(writeError);
    CHECK(json.contains("\"number\""));
    CHECK(json.contains("\"filterMinValue\""));
    CHECK(json.contains("\"filterMaxValue\""));

    LogConfiguration loaded;
    const auto readError = glz::read_json(loaded, json);
    REQUIRE_FALSE(readError);

    REQUIRE(loaded.filters.size() == 3);
    CHECK(loaded.filters[0].type == LogConfiguration::LogFilter::Type::Number);
    REQUIRE(loaded.filters[0].filterMinValue.has_value());
    REQUIRE(loaded.filters[0].filterMaxValue.has_value());
    CHECK(*loaded.filters[0].filterMinValue == Catch::Approx(-2.5));
    CHECK(*loaded.filters[0].filterMaxValue == Catch::Approx(17.25));

    CHECK_FALSE(loaded.filters[1].filterMinValue.has_value());
    REQUIRE(loaded.filters[1].filterMaxValue.has_value());
    CHECK(*loaded.filters[1].filterMaxValue == Catch::Approx(100.0));

    REQUIRE(loaded.filters[2].filterMinValue.has_value());
    CHECK(*loaded.filters[2].filterMinValue == Catch::Approx(0.0));
    CHECK_FALSE(loaded.filters[2].filterMaxValue.has_value());
}

TEST_CASE("Round-trip LogFilter with Type::Boolean", "[log_configuration][boolean]")
{
    LogConfiguration original;
    LogConfiguration::LogFilter filter;
    filter.type = LogConfiguration::LogFilter::Type::Boolean;
    filter.row = 0;
    filter.filterValues = {"true"};
    original.filters.push_back(filter);

    std::string json;
    const auto writeError = glz::write_json(original, json);
    REQUIRE_FALSE(writeError);
    CHECK(json.contains("\"boolean\""));

    LogConfiguration loaded;
    const auto readError = glz::read_json(loaded, json);
    REQUIRE_FALSE(readError);

    REQUIRE(loaded.filters.size() == 1);
    CHECK(loaded.filters[0].type == LogConfiguration::LogFilter::Type::Boolean);
    CHECK(loaded.filters[0].filterValues == std::vector<std::string>{"true"});
}

TEST_CASE(
    "Legacy lowerCamelCase JSON keys still load after enum rename to UpperCamelCase",
    "[log_configuration][wire_format_compat]"
)
{
    // Hand-written JSON in the historical on-disk shape (lowerCamelCase
    // enum keys for `Type`, `LogFilter::Type`, and `LogFilter::Match`).
    // The C++ enumerators are now UpperCamelCase, but the Glaze meta
    // pins the wire format so existing saved configurations keep
    // working. Every `Type`, `LogFilter::Type`, and `LogFilter::Match`
    // variant is exercised so renaming any one would break the test.
    constexpr std::string_view LEGACY_JSON = R"({
        "columns": [
            {"header":"b","keys":["b"],"printFormat":"{}","type":"any","parseFormats":[]},
            {"header":"c","keys":["c"],"printFormat":"{}","type":"string","parseFormats":[]},
            {"header":"d","keys":["d"],"printFormat":"{}","type":"integer","parseFormats":[]},
            {"header":"e","keys":["e"],"printFormat":"{}","type":"floating","parseFormats":[]},
            {"header":"f","keys":["f"],"printFormat":"{}","type":"number","parseFormats":[]},
            {"header":"g","keys":["g"],"printFormat":"{}","type":"time","parseFormats":[]},
            {"header":"h","keys":["h"],"printFormat":"{}","type":"enumeration","parseFormats":[]}
        ],
        "filters": [
            {"type":"string","row":0,"filterString":"foo","matchType":"exactly","filterValues":[]},
            {"type":"string","row":1,"filterString":"bar","matchType":"contains","filterValues":[]},
            {"type":"string","row":2,"filterString":"^baz$","matchType":"regularExpression","filterValues":[]},
            {"type":"string","row":3,"filterString":"qux*","matchType":"wildcard","filterValues":[]},
            {"type":"time","row":4,"filterBegin":1000,"filterEnd":2000,"filterValues":[]},
            {"type":"enumeration","row":5,"filterValues":["info","warn"]}
        ]
    })";

    LogConfiguration loaded;
    const auto readError = glz::read_json(loaded, LEGACY_JSON);
    REQUIRE_FALSE(readError);

    using Type = LogConfiguration::Type;
    REQUIRE(loaded.columns.size() == 7);
    CHECK(loaded.columns[0].type == Type::Any);
    CHECK(loaded.columns[1].type == Type::String);
    CHECK(loaded.columns[2].type == Type::Integer);
    CHECK(loaded.columns[3].type == Type::Floating);
    CHECK(loaded.columns[4].type == Type::Number);
    CHECK(loaded.columns[5].type == Type::Time);
    CHECK(loaded.columns[6].type == Type::Enumeration);

    using FilterType = LogConfiguration::LogFilter::Type;
    using Match = LogConfiguration::LogFilter::Match;
    REQUIRE(loaded.filters.size() == 6);
    CHECK(loaded.filters[0].type == FilterType::String);
    REQUIRE(loaded.filters[0].matchType.has_value());
    CHECK(*loaded.filters[0].matchType == Match::Exactly);
    REQUIRE(loaded.filters[1].matchType.has_value());
    CHECK(*loaded.filters[1].matchType == Match::Contains);
    REQUIRE(loaded.filters[2].matchType.has_value());
    CHECK(*loaded.filters[2].matchType == Match::RegularExpression);
    REQUIRE(loaded.filters[3].matchType.has_value());
    CHECK(*loaded.filters[3].matchType == Match::Wildcard);
    CHECK(loaded.filters[4].type == FilterType::Time);
    CHECK(loaded.filters[5].type == FilterType::Enumeration);

    // Re-serialise and confirm the wire format keeps the original keys.
    std::string roundTripJson;
    const auto writeError = glz::write_json(loaded, roundTripJson);
    REQUIRE_FALSE(writeError);
    CHECK(roundTripJson.contains("\"any\""));
    CHECK(roundTripJson.contains("\"enumeration\""));
    CHECK(roundTripJson.contains("\"regularExpression\""));
    CHECK_FALSE(roundTripJson.contains("\"Any\""));
    CHECK_FALSE(roundTripJson.contains("\"Enumeration\""));
    CHECK_FALSE(roundTripJson.contains("\"RegularExpression\""));
}

TEST_CASE(
    "LogFilter::Type::Number and Type::Boolean load via lowerCamelCase JSON keys",
    "[log_configuration][wire_format_compat][number][boolean]"
)
{
    // Hand-written JSON pins the lowerCamelCase wire format for the
    // new filter variants so future renames stay back-compat.
    constexpr std::string_view JSON = R"({
        "columns": [
            {"header":"value","keys":["value"],"printFormat":"{}","type":"number","parseFormats":[]},
            {"header":"flag","keys":["flag"],"printFormat":"{}","type":"boolean","parseFormats":[]}
        ],
        "filters": [
            {"type":"number","row":0,"filterMinValue":1.5,"filterMaxValue":5.0,"filterValues":[]},
            {"type":"boolean","row":1,"filterValues":["true","false"]}
        ]
    })";

    LogConfiguration loaded;
    const auto readError = glz::read_json(loaded, JSON);
    REQUIRE_FALSE(readError);

    using Type = LogConfiguration::Type;
    REQUIRE(loaded.columns.size() == 2);
    CHECK(loaded.columns[0].type == Type::Number);
    CHECK(loaded.columns[1].type == Type::Boolean);

    using FilterType = LogConfiguration::LogFilter::Type;
    REQUIRE(loaded.filters.size() == 2);
    CHECK(loaded.filters[0].type == FilterType::Number);
    REQUIRE(loaded.filters[0].filterMinValue.has_value());
    REQUIRE(loaded.filters[0].filterMaxValue.has_value());
    CHECK(*loaded.filters[0].filterMinValue == Catch::Approx(1.5));
    CHECK(*loaded.filters[0].filterMaxValue == Catch::Approx(5.0));

    CHECK(loaded.filters[1].type == FilterType::Boolean);
    CHECK(loaded.filters[1].filterValues == std::vector<std::string>{"true", "false"});
}

TEST_CASE(
    "Column::Type::Level survives a JSON round-trip alongside levelMapping",
    "[log_configuration][wire_format_compat][level]"
)
{
    // Pin the wire tokens (`"level"`, `"levelMapping"`). The actual
    // pair encoding glaze uses is an internal default, so the test
    // round-trips through `write_json` -> `read_json` instead of
    // matching bytes.
    LogConfiguration original;
    LogConfiguration::Column column;
    column.header = "severity";
    column.keys = {"severity"};
    column.printFormat = "{}";
    column.type = LogConfiguration::Type::Level;
    column.parseFormats = {};
    column.levelMapping = {
        {"NOTICE", "Info"},
        {"30", "Info"},
    };
    original.columns.push_back(column);

    std::string json;
    const auto writeError = glz::write_json(original, json);
    REQUIRE_FALSE(writeError);

    // Public wire tokens. `"level"` is locked by the glaze meta;
    // the field name comes straight from the struct.
    CHECK(json.contains("\"type\":\"level\""));
    CHECK(json.contains("\"levelMapping\""));
    CHECK(json.contains("\"NOTICE\""));
    CHECK(json.contains("\"Info\""));
    CHECK(json.contains("\"30\""));

    LogConfiguration loaded;
    const auto readError = glz::read_json(loaded, json);
    REQUIRE_FALSE(readError);

    REQUIRE(loaded.columns.size() == 1);
    CHECK(loaded.columns[0].type == LogConfiguration::Type::Level);
    REQUIRE(loaded.columns[0].levelMapping.size() == 2);
    CHECK(loaded.columns[0].levelMapping[0].first == "NOTICE");
    CHECK(loaded.columns[0].levelMapping[0].second == "Info");
    CHECK(loaded.columns[0].levelMapping[1].first == "30");
    CHECK(loaded.columns[0].levelMapping[1].second == "Info");

    const auto &mapping = loaded.columns[0].levelMapping;
    CHECK(ResolveLevel("NOTICE", mapping) == LogLevel::Info);
    CHECK(ResolveLevel("30", mapping) == LogLevel::Info);
}

TEST_CASE(
    "LogConfigurationManager::SetColumnHeader renames the display label without touching keys",
    "[LogConfigurationManager][column_header]"
)
{
    // `header` is the display string the GUI shows; `keys` are the
    // parser identifiers and must stay untouched so the existing
    // column binding survives. The mutator is the only path the
    // Column Editor takes to rename a column, so a hand-on-the-wheel
    // test pins that one-way invariant.
    LogConfigurationManager manager;
    manager.AppendKeys({"abc", "xyz"});
    REQUIRE(manager.Configuration().columns.size() == 2);

    manager.SetColumnHeader(0, "Alpha");
    CHECK(manager.Configuration().columns[0].header == "Alpha");
    CHECK(manager.Configuration().columns[0].keys == std::vector<std::string>{"abc"});
    CHECK(manager.Configuration().columns[1].header == "xyz");

    // Empty rename is intentionally allowed -- users can clear the
    // header without us second-guessing them; the table falls back
    // to the keys via `LogTable::GetHeader`.
    manager.SetColumnHeader(1, "");
    CHECK(manager.Configuration().columns[1].header.empty());

    // Out-of-range index must be a silent no-op (matches the rest of
    // the SetColumn* family). The valid columns stay untouched.
    manager.SetColumnHeader(99, "Bogus");
    REQUIRE(manager.Configuration().columns.size() == 2);
    CHECK(manager.Configuration().columns[0].header == "Alpha");
}

TEST_CASE("Column::visible round-trips through Save/Load", "[LogConfigurationManager][column_visibility]")
{
    // The hidden-column flag must survive Save / Load.
    const TestLogConfiguration testConfiguration;

    {
        LogConfiguration configuration;
        configuration.columns.push_back(
            LogConfiguration::Column{
                .header = "shown",
                .keys = {"shown"},
                .printFormat = "{}",
                .type = LogConfiguration::Type::String,
                .parseFormats = {},
                .visible = true,
            }
        );
        configuration.columns.push_back(
            LogConfiguration::Column{
                .header = "hidden",
                .keys = {"hidden"},
                .printFormat = "{}",
                .type = LogConfiguration::Type::String,
                .parseFormats = {},
                .visible = false,
            }
        );
        testConfiguration.Write(configuration);
    }

    LogConfigurationManager manager;
    manager.Load(testConfiguration.GetFilePath());
    REQUIRE(manager.Configuration().columns.size() == 2);
    CHECK(manager.Configuration().columns[0].visible);
    CHECK_FALSE(manager.Configuration().columns[1].visible);

    // Save back and confirm the flag survives the round-trip.
    const TestLogConfiguration roundTrip;
    manager.Save(roundTrip.GetFilePath());

    LogConfigurationManager reloaded;
    reloaded.Load(roundTrip.GetFilePath());
    REQUIRE(reloaded.Configuration().columns.size() == 2);
    CHECK(reloaded.Configuration().columns[0].visible);
    CHECK_FALSE(reloaded.Configuration().columns[1].visible);
}

TEST_CASE(
    "Legacy JSON without `visible` key loads as visible == true",
    "[log_configuration][wire_format_compat][column_visibility]"
)
{
    // Pre-`Column::visible` configs must keep loading. Glaze
    // tolerates missing keys; the field defaults to `true`.
    constexpr std::string_view LEGACY_JSON = R"({
        "columns": [
            {"header":"a","keys":["a"],"printFormat":"{}","type":"any","parseFormats":[]}
        ],
        "filters": []
    })";

    LogConfiguration loaded;
    const auto readError = glz::read_json(loaded, LEGACY_JSON);
    REQUIRE_FALSE(readError);
    REQUIRE(loaded.columns.size() == 1);
    CHECK(loaded.columns[0].visible);
    CHECK(loaded.columns[0].autoDetect);
}

TEST_CASE(
    "LogConfigurationManager::MoveColumn remaps LogFilter::row through the permutation",
    "[LogConfigurationManager][move_column][filter_row_remap]"
)
{
    // Four columns + one filter per column; each filter's `row`
    // starts equal to its column index.
    LogConfigurationManager manager;
    manager.AppendKeys({"a", "b", "c", "d"});
    REQUIRE(manager.Configuration().columns.size() == 4);

    // Bypass `AddFilter` (opens an editor): write filters to disk
    // and re-load.
    {
        LogConfiguration configuration;
        configuration.columns = manager.Configuration().columns;
        for (int row = 0; row < 4; ++row)
        {
            configuration.filters.push_back(
                LogConfiguration::LogFilter{
                    .type = LogConfiguration::LogFilter::Type::String,
                    .row = row,
                    .filterString = std::string{"x"},
                    .matchType = LogConfiguration::LogFilter::Match::Contains,
                    .filterBegin = std::nullopt,
                    .filterEnd = std::nullopt,
                    .filterMinValue = std::nullopt,
                    .filterMaxValue = std::nullopt,
                    .filterValues = {},
                }
            );
        }
        const TestLogConfiguration testConfiguration;
        testConfiguration.Write(configuration);
        manager.Load(testConfiguration.GetFilePath());
    }
    REQUIRE(manager.Configuration().filters.size() == 4);

    SECTION("Right-to-left move (3 -> 0) shifts everything else right")
    {
        manager.MoveColumn(3, 0);
        const auto &filters = manager.Configuration().filters;
        REQUIRE(filters.size() == 4);
        // Filters keep their order (created in column order); only
        // `row` follows the permutation:
        //   col 0 ('a') -> row 1
        //   col 1 ('b') -> row 2
        //   col 2 ('c') -> row 3
        //   col 3 ('d') -> row 0
        CHECK(filters[0].row == 1);
        CHECK(filters[1].row == 2);
        CHECK(filters[2].row == 3);
        CHECK(filters[3].row == 0);
    }

    SECTION("Left-to-right move (0 -> 3) shifts the in-between columns left")
    {
        manager.MoveColumn(0, 3);
        const auto &filters = manager.Configuration().filters;
        REQUIRE(filters.size() == 4);
        // Permutation:
        //   col 0 ('a') -> row 3
        //   col 1 ('b') -> row 0
        //   col 2 ('c') -> row 1
        //   col 3 ('d') -> row 2
        CHECK(filters[0].row == 3);
        CHECK(filters[1].row == 0);
        CHECK(filters[2].row == 1);
        CHECK(filters[3].row == 2);
    }

    SECTION("Adjacent swap (1 -> 2) only touches the two adjacent rows")
    {
        manager.MoveColumn(1, 2);
        const auto &filters = manager.Configuration().filters;
        REQUIRE(filters.size() == 4);
        CHECK(filters[0].row == 0);
        CHECK(filters[1].row == 2);
        CHECK(filters[2].row == 1);
        CHECK(filters[3].row == 3);
    }

    SECTION("Out-of-range / no-op move leaves rows unchanged")
    {
        manager.MoveColumn(0, 0);
        manager.MoveColumn(0, 99);
        manager.MoveColumn(99, 0);
        const auto &filters = manager.Configuration().filters;
        REQUIRE(filters.size() == 4);
        CHECK(filters[0].row == 0);
        CHECK(filters[1].row == 1);
        CHECK(filters[2].row == 2);
        CHECK(filters[3].row == 3);
    }
}

TEST_CASE(
    "LogConfigurationManager::MoveColumn remaps Sort::columnIndex through the permutation",
    "[LogConfigurationManager][move_column][sort_remap]"
)
{
    // Filters and the persisted sort indicator share the same column
    // identifier, so they must ride the same rotation. Without the
    // sort remap, a `SaveScope::Full` round-trip after a column drag
    // would land the indicator on the wrong column.
    LogConfigurationManager manager;
    manager.AppendKeys({"a", "b", "c", "d"});
    REQUIRE(manager.Configuration().columns.size() == 4);

    SECTION("Sort on the dragged column travels with the move")
    {
        // Index 2 ("c") sorts descending; drag "c" to the front.
        manager.SetSort(LogConfiguration::Sort{.columnIndex = 2, .descending = true});

        manager.MoveColumn(2, 0);
        // "c" is now at index 0; the indicator must follow.
        CHECK(manager.Configuration().sort.columnIndex == 0);
        // Direction is preserved.
        CHECK(manager.Configuration().sort.descending == true);
    }

    SECTION("Sort on a downstream column slides up when the drag crosses it")
    {
        manager.SetSort(LogConfiguration::Sort{.columnIndex = 3, .descending = false});

        // Move "a" (0) to position 3 -- "d" shifts left to index 2.
        manager.MoveColumn(0, 3);
        CHECK(manager.Configuration().sort.columnIndex == 2);
    }

    SECTION("Sort indicator outside the rotation window stays put")
    {
        manager.SetSort(LogConfiguration::Sort{.columnIndex = 0, .descending = false});

        manager.MoveColumn(2, 3);
        CHECK(manager.Configuration().sort.columnIndex == 0);
    }

    SECTION("No-sort sentinel (-1) survives a move")
    {
        manager.SetSort(LogConfiguration::Sort{.columnIndex = -1, .descending = false});

        manager.MoveColumn(0, 2);
        CHECK(manager.Configuration().sort.columnIndex == -1);
    }
}

TEST_CASE(
    "RemapColumnIndexAfterMove permutation matches MoveColumn's internal logic",
    "[LogConfigurationManager][move_column]"
)
{
    // The app uses this static helper to remap its runtime filter
    // map; keep it in lockstep with `MoveColumn`'s rotation.
    using Mgr = LogConfigurationManager;
    CHECK(Mgr::RemapColumnIndexAfterMove(0, 0, 0) == 0);
    CHECK(Mgr::RemapColumnIndexAfterMove(2, 3, 0) == 3);
    CHECK(Mgr::RemapColumnIndexAfterMove(3, 3, 0) == 0);
    CHECK(Mgr::RemapColumnIndexAfterMove(0, 3, 0) == 1);
    CHECK(Mgr::RemapColumnIndexAfterMove(0, 0, 3) == 3);
    CHECK(Mgr::RemapColumnIndexAfterMove(1, 0, 3) == 0);
    CHECK(Mgr::RemapColumnIndexAfterMove(3, 0, 3) == 2);
    CHECK(Mgr::RemapColumnIndexAfterMove(5, 1, 2) == 5);
}

TEST_CASE(
    "Update's auto-promoted timestamp bubble remaps persisted LogFilter::row",
    "[LogConfigurationManager][update][filter_row_remap]"
)
{
    // `Update` appends auto-promoted timestamp columns and bubbles
    // them to index 0. Persisted `LogFilter::row` must follow the
    // bubble. Streaming-side coverage lives in
    // `TestSourceColumnMoveRemapsRuntimeFilters`.
    LogConfigurationManager manager;
    manager.AppendKeys({"regular_a", "regular_b"});
    REQUIRE(manager.Configuration().columns.size() == 2);
    REQUIRE(manager.Configuration().columns[0].header == "regular_a");
    REQUIRE(manager.Configuration().columns[1].header == "regular_b");

    // Filters pointing at the two existing columns; both must
    // follow their column through the bubble.
    {
        LogConfiguration configuration;
        configuration.columns = manager.Configuration().columns;
        configuration.filters.push_back(
            LogConfiguration::LogFilter{
                .type = LogConfiguration::LogFilter::Type::String,
                .row = 0,
                .filterString = std::string{"x"},
                .matchType = LogConfiguration::LogFilter::Match::Contains,
                .filterBegin = std::nullopt,
                .filterEnd = std::nullopt,
                .filterMinValue = std::nullopt,
                .filterMaxValue = std::nullopt,
                .filterValues = {},
            }
        );
        configuration.filters.push_back(
            LogConfiguration::LogFilter{
                .type = LogConfiguration::LogFilter::Type::String,
                .row = 1,
                .filterString = std::string{"y"},
                .matchType = LogConfiguration::LogFilter::Match::Contains,
                .filterBegin = std::nullopt,
                .filterEnd = std::nullopt,
                .filterMinValue = std::nullopt,
                .filterMaxValue = std::nullopt,
                .filterValues = {},
            }
        );
        const TestLogConfiguration testConfiguration;
        testConfiguration.Write(configuration);
        manager.Load(testConfiguration.GetFilePath());
    }
    REQUIRE(manager.Configuration().filters.size() == 2);

    // `Update` with a fresh `timestamp` key: appended at index 2
    // then bubbled to index 0; existing columns shift right.
    const TestLogFile testLogFile;
    auto source = testLogFile.CreateFileLineSource();
    KeyIndex testKeys;
    std::vector<LogLine> testLines;
    testLines.emplace_back(LogMap{{"timestamp", std::string("2023-01-01T12:00:00Z")}}, testKeys, *source, 0);
    const LogData logData(std::move(source), std::move(testLines), std::move(testKeys));

    manager.Update(logData);

    REQUIRE(manager.Configuration().columns.size() == 3);
    CHECK(manager.Configuration().columns[0].header == "timestamp");
    CHECK(manager.Configuration().columns[0].type == LogConfiguration::Type::Time);
    CHECK(manager.Configuration().columns[1].header == "regular_a");
    CHECK(manager.Configuration().columns[2].header == "regular_b");

    // Filters followed the bubble: row 0 -> row 1, row 1 -> row 2.
    // Without the remap they would still report pre-bubble indices
    // and silently target the wrong columns.
    REQUIRE(manager.Configuration().filters.size() == 2);
    CHECK(manager.Configuration().filters[0].row == 1);
    CHECK(manager.Configuration().filters[0].filterString == std::string{"x"});
    CHECK(manager.Configuration().filters[1].row == 2);
    CHECK(manager.Configuration().filters[1].filterString == std::string{"y"});
}

TEST_CASE("BubbleLevelColumnToCanonicalPosition mirrors the Time bubble", "[LogConfigurationManager][level_bubble]")
{
    // Manager-level coverage of the free helper. Catches rotation
    // regressions independent of `LogTable::MaybePromoteToLevel`
    // (which is covered in `test_log_table.cpp`).
    SECTION("Move from end to canonical position 1")
    {
        LogConfigurationManager manager;
        manager.AppendKeys({"col_zero", "col_one", "level"});
        REQUIRE(manager.Configuration().columns.size() == 3);
        REQUIRE(manager.Configuration().columns[2].header == "level");

        BubbleLevelColumnToCanonicalPosition(manager, 2);

        REQUIRE(manager.Configuration().columns.size() == 3);
        CHECK(manager.Configuration().columns[0].header == "col_zero");
        CHECK(manager.Configuration().columns[1].header == "level");
        CHECK(manager.Configuration().columns[2].header == "col_one");
    }

    SECTION("No-op when already at canonical position")
    {
        LogConfigurationManager manager;
        manager.AppendKeys({"col_zero", "level", "col_two"});

        BubbleLevelColumnToCanonicalPosition(manager, 1);

        CHECK(manager.Configuration().columns[0].header == "col_zero");
        CHECK(manager.Configuration().columns[1].header == "level");
        CHECK(manager.Configuration().columns[2].header == "col_two");
    }

    SECTION("No-op for a single-column configuration")
    {
        // Only column -- nothing to swap with; must not throw.
        LogConfigurationManager manager;
        manager.AppendKeys({"level"});

        BubbleLevelColumnToCanonicalPosition(manager, 0);

        REQUIRE(manager.Configuration().columns.size() == 1);
        CHECK(manager.Configuration().columns[0].header == "level");
    }

    SECTION("Move from position 0 to position 1 in a two-column config")
    {
        LogConfigurationManager manager;
        manager.AppendKeys({"level", "col_one"});

        BubbleLevelColumnToCanonicalPosition(manager, 0);

        REQUIRE(manager.Configuration().columns.size() == 2);
        CHECK(manager.Configuration().columns[0].header == "col_one");
        CHECK(manager.Configuration().columns[1].header == "level");
    }

    SECTION("Out-of-range index is a no-op")
    {
        LogConfigurationManager manager;
        manager.AppendKeys({"col_zero", "col_one"});

        BubbleLevelColumnToCanonicalPosition(manager, 99);

        REQUIRE(manager.Configuration().columns.size() == 2);
        CHECK(manager.Configuration().columns[0].header == "col_zero");
        CHECK(manager.Configuration().columns[1].header == "col_one");
    }

    SECTION("Persisted filter row follows the bubble")
    {
        // Proves the helper delegates to `MoveColumn` rather than
        // open-coding the rotation, so `filters[*].row` stays in sync.
        LogConfigurationManager manager;
        manager.AppendKeys({"col_zero", "col_one", "level"});

        // Pin a filter on `level` so the remap is observable.
        LogConfiguration cfg = manager.Configuration();
        cfg.filters.push_back(
            LogConfiguration::LogFilter{
                .type = LogConfiguration::LogFilter::Type::String,
                .row = 2,
                .filterString = std::string{"warn"},
                .matchType = LogConfiguration::LogFilter::Match::Contains,
                .filterBegin = std::nullopt,
                .filterEnd = std::nullopt,
                .filterMinValue = std::nullopt,
                .filterMaxValue = std::nullopt,
                .filterValues = {},
            }
        );
        manager.SetConfiguration(std::move(cfg));

        BubbleLevelColumnToCanonicalPosition(manager, 2);

        REQUIRE(manager.Configuration().filters.size() == 1);
        CHECK(manager.Configuration().filters[0].row == 1);
    }
}

TEST_CASE("Failed Load leaves the previous configuration intact", "[LogConfigurationManager][atomic_load]")
{
    // A malformed file must not corrupt the previously loaded
    // configuration. Glaze writes member-by-member, so a direct read
    // into `mConfiguration` would half-populate it on parse failure
    // -- and `TryLoadAsConfiguration`'s fall-through would then
    // start streaming over corrupt state.
    LogConfigurationManager manager;

    // Stage 1: load a known-good configuration as the baseline.
    {
        LogConfiguration good;
        good.columns.push_back(
            LogConfiguration::Column{
                .header = "good_a",
                .keys = {"a"},
                .printFormat = "{}",
                .type = LogConfiguration::Type::String,
                .parseFormats = {},
                .visible = true,
            }
        );
        good.columns.push_back(
            LogConfiguration::Column{
                .header = "good_b",
                .keys = {"b"},
                .printFormat = "{}",
                .type = LogConfiguration::Type::Integer,
                .parseFormats = {},
                .visible = false,
            }
        );
        const TestLogConfiguration goodFile;
        goodFile.Write(good);
        manager.Load(goodFile.GetFilePath());
    }
    REQUIRE(manager.Configuration().columns.size() == 2);
    REQUIRE(manager.Configuration().columns[0].header == "good_a");
    REQUIRE(manager.Configuration().columns[1].header == "good_b");
    REQUIRE_FALSE(manager.Configuration().columns[1].visible);

    // Stage 2: malformed JSON that parses partway through so Glaze
    // is guaranteed to touch the live struct before erroring.
    constexpr std::string_view BROKEN_JSON = R"({
        "columns": [
            {"header":"new_a","keys":["a"],"printFormat":"{}","type":"string","parseFormats":[]},
            {"header":"new_b","keys":["b"],"printFormat":"{}","type": NOT_A_VALID_VALUE]
        ],
        "filters": []
    })";
    const TestLogConfiguration brokenFile;
    {
        std::ofstream stream(brokenFile.GetFilePath(), std::ios::binary);
        REQUIRE(stream.is_open());
        stream << BROKEN_JSON;
    }
    CHECK_THROWS_AS(manager.Load(brokenFile.GetFilePath()), std::runtime_error);

    // Stage 3: the previous configuration must be byte-identical
    // to its pre-load state. Without atomic load, `columns[0]`
    // would already read as `new_a`.
    REQUIRE(manager.Configuration().columns.size() == 2);
    CHECK(manager.Configuration().columns[0].header == "good_a");
    CHECK(manager.Configuration().columns[1].header == "good_b");
    CHECK(manager.Configuration().columns[0].visible);
    CHECK_FALSE(manager.Configuration().columns[1].visible);
}

TEST_CASE("IsLogLevelKey recognises canonical level aliases case-insensitively", "[log_configuration][log_level]")
{
    // Long-form / classic.
    CHECK(IsLogLevelKey("level"));
    CHECK(IsLogLevelKey("Level"));
    CHECK(IsLogLevelKey("LEVEL"));
    CHECK(IsLogLevelKey("severity"));
    CHECK(IsLogLevelKey("Severity"));
    CHECK(IsLogLevelKey("loglevel"));
    CHECK(IsLogLevelKey("LogLevel"));
    CHECK(IsLogLevelKey("log_level"));
    CHECK(IsLogLevelKey("LOG_LEVEL"));
    CHECK(IsLogLevelKey("log.level"));
    CHECK(IsLogLevelKey("Log.Level"));
    CHECK(IsLogLevelKey("lvl"));
    CHECK(IsLogLevelKey("LVL"));
    CHECK(IsLogLevelKey("levelname"));
    CHECK(IsLogLevelKey("LevelName"));
    CHECK(IsLogLevelKey("priority"));
    CHECK(IsLogLevelKey("Priority"));

    // Short forms. The dictionary-content check in
    // `MaybePromoteToLevel` is the safety net against false positives.
    CHECK(IsLogLevelKey("l"));
    CHECK(IsLogLevelKey("L"));
    CHECK(IsLogLevelKey("lv"));
    CHECK(IsLogLevelKey("LV"));
    CHECK(IsLogLevelKey("lev"));
    CHECK(IsLogLevelKey("Lev"));
    CHECK(IsLogLevelKey("sev"));
    CHECK(IsLogLevelKey("SEV"));
    CHECK(IsLogLevelKey("s"));
    CHECK(IsLogLevelKey("S"));
    CHECK(IsLogLevelKey("loglvl"));
    CHECK(IsLogLevelKey("LogLvl"));

    // OpenTelemetry / ECS / GCP.
    CHECK(IsLogLevelKey("severity_text"));
    CHECK(IsLogLevelKey("Severity_Text"));
    CHECK(IsLogLevelKey("severity.text"));
    CHECK(IsLogLevelKey("severitytext"));
    CHECK(IsLogLevelKey("SeverityText"));
    CHECK(IsLogLevelKey("log_severity"));
    CHECK(IsLogLevelKey("log.severity"));
    CHECK(IsLogLevelKey("logseverity"));
    CHECK(IsLogLevelKey("LogSeverity"));

    // Separator variants of `levelname`.
    CHECK(IsLogLevelKey("level_name"));
    CHECK(IsLogLevelKey("Level_Name"));
    CHECK(IsLogLevelKey("level.name"));

    // Structured-JSON convention.
    CHECK(IsLogLevelKey("@level"));
    CHECK(IsLogLevelKey("@LEVEL"));

    // Non-matches: exact equality, not prefix / substring.
    CHECK_FALSE(IsLogLevelKey("status"));
    CHECK_FALSE(IsLogLevelKey("timestamp"));
    CHECK_FALSE(IsLogLevelKey(""));
    CHECK_FALSE(IsLogLevelKey("levelish"));
    CHECK_FALSE(IsLogLevelKey("la"));
    CHECK_FALSE(IsLogLevelKey("lengthy"));
    CHECK_FALSE(IsLogLevelKey("severities"));
    CHECK_FALSE(IsLogLevelKey("ll"));
    CHECK_FALSE(IsLogLevelKey(" level"));
    CHECK_FALSE(IsLogLevelKey("level "));
}

TEST_CASE("FirstTimeColumnIndex returns the first Type::Time column or -1", "[log_configuration][time_column]")
{
    using Type = LogConfiguration::Type;

    const LogConfiguration empty;
    CHECK(FirstTimeColumnIndex(empty) == -1);

    LogConfiguration noTime;
    noTime.columns.push_back({.header = "msg", .type = Type::String});
    noTime.columns.push_back({.header = "level", .type = Type::Level});
    CHECK(FirstTimeColumnIndex(noTime) == -1);

    LogConfiguration single;
    single.columns.push_back({.header = "msg", .type = Type::String});
    single.columns.push_back({.header = "ts", .type = Type::Time});
    single.columns.push_back({.header = "level", .type = Type::Level});
    CHECK(FirstTimeColumnIndex(single) == 1);

    // First wins: callers rely on this "canonical column" behaviour.
    LogConfiguration twoTimes;
    twoTimes.columns.push_back({.header = "started_at", .type = Type::Time});
    twoTimes.columns.push_back({.header = "msg", .type = Type::String});
    twoTimes.columns.push_back({.header = "ended_at", .type = Type::Time});
    CHECK(FirstTimeColumnIndex(twoTimes) == 0);

    // Index 0 is a real result, not the "not found" sentinel.
    LogConfiguration headTime;
    headTime.columns.push_back({.header = "ts", .type = Type::Time});
    CHECK(FirstTimeColumnIndex(headTime) == 0);
}

TEST_CASE(
    "LogConfiguration serialises Type::Level and Column::levelMapping round-trip",
    "[log_configuration][log_level][serialization]"
)
{
    LogConfiguration cfg;
    LogConfiguration::Column column;
    column.header = "severity";
    column.keys = {"severity"};
    column.printFormat = "{}";
    column.type = LogConfiguration::Type::Level;
    column.parseFormats = {};
    column.levelMapping = {
        {"NOTICE", "Info"},
        {"PANIC", "Fatal"},
    };
    cfg.columns.push_back(column);

    const TestLogConfiguration file;
    file.Write(cfg);

    LogConfigurationManager loaded;
    loaded.Load(file.GetFilePath());

    REQUIRE(loaded.Configuration().columns.size() == 1);
    const auto &restored = loaded.Configuration().columns[0];
    CHECK(restored.header == "severity");
    CHECK(restored.type == LogConfiguration::Type::Level);
    REQUIRE(restored.levelMapping.size() == 2);
    CHECK(restored.levelMapping[0].first == "NOTICE");
    CHECK(restored.levelMapping[0].second == "Info");
    CHECK(restored.levelMapping[1].first == "PANIC");
    CHECK(restored.levelMapping[1].second == "Fatal");
}

TEST_CASE("ParseLevelName matches the documented alias table", "[log_level]")
{
    // Every alias in `BUILTIN_ALIASES`, plus mixed-case spellings for
    // each new short-form/library-specific entry. Keeps the table in
    // sync with the header docstring.
    using Expected = std::pair<std::string_view, LogLevel>;
    constexpr std::array<Expected, 52> CASES = {{
        // Trace
        {"trace", LogLevel::Trace},
        {"TRACE", LogLevel::Trace},
        {"trc", LogLevel::Trace},
        {"Trc", LogLevel::Trace},
        {"t", LogLevel::Trace},
        {"T", LogLevel::Trace},
        {"finer", LogLevel::Trace},
        {"Finer", LogLevel::Trace},
        {"finest", LogLevel::Trace},
        {"FINEST", LogLevel::Trace},
        {"silly", LogLevel::Trace},
        {"Silly", LogLevel::Trace},
        // Debug
        {"debug", LogLevel::Debug},
        {"dbg", LogLevel::Debug},
        {"d", LogLevel::Debug},
        {"D", LogLevel::Debug},
        {"verbose", LogLevel::Debug},
        {"vrb", LogLevel::Debug},
        {"VRB", LogLevel::Debug},
        {"v", LogLevel::Debug},
        {"V", LogLevel::Debug},
        {"fine", LogLevel::Debug},
        {"Fine", LogLevel::Debug},
        // Info
        {"info", LogLevel::Info},
        {"inf", LogLevel::Info},
        {"INF", LogLevel::Info},
        {"i", LogLevel::Info},
        {"I", LogLevel::Info},
        {"Information", LogLevel::Info},
        {"informational", LogLevel::Info},
        {"notice", LogLevel::Info},
        // Warn
        {"warn", LogLevel::Warn},
        {"WARNING", LogLevel::Warn},
        {"wrn", LogLevel::Warn},
        {"WRN", LogLevel::Warn},
        {"w", LogLevel::Warn},
        {"W", LogLevel::Warn},
        // Error
        {"error", LogLevel::Error},
        {"ERR", LogLevel::Error},
        {"severe", LogLevel::Error},
        {"e", LogLevel::Error},
        {"E", LogLevel::Error},
        // Fatal
        {"fatal", LogLevel::Fatal},
        {"critical", LogLevel::Fatal},
        {"crit", LogLevel::Fatal},
        {"emerg", LogLevel::Fatal},
        {"ftl", LogLevel::Fatal},
        {"FTL", LogLevel::Fatal},
        {"f", LogLevel::Fatal},
        {"F", LogLevel::Fatal},
        {"fault", LogLevel::Fatal},
        {"Fault", LogLevel::Fatal},
    }};
    for (const auto &[input, expected] : CASES)
    {
        CAPTURE(input);
        const auto actual = ParseLevelName(input);
        REQUIRE(actual.has_value());
        CHECK(*actual == expected);
    }

    CHECK_FALSE(ParseLevelName("").has_value());
    CHECK_FALSE(ParseLevelName("INFOZ").has_value()); // near-miss of "info"; alias table is exact-match only
    CHECK_FALSE(ParseLevelName("unknown").has_value());
    CHECK_FALSE(ParseLevelName("\\t info").has_value());
    // Deliberately *not* built-in (see header + README). Catches
    // drift if they're added without updating the docs.
    CHECK_FALSE(ParseLevelName("config").has_value());
    CHECK_FALSE(ParseLevelName("default").has_value());
    CHECK_FALSE(ParseLevelName("http").has_value());
    // Numeric-string levels: handled via `levelMapping`, not built-in.
    CHECK_FALSE(ParseLevelName("10").has_value());
    CHECK_FALSE(ParseLevelName("30").has_value());
    CHECK_FALSE(ParseLevelName("0").has_value());
}

TEST_CASE("CanonicalLevelName mirrors the LogLevel enum", "[log_level]")
{
    CHECK(CanonicalLevelName(LogLevel::Trace) == "Trace");
    CHECK(CanonicalLevelName(LogLevel::Debug) == "Debug");
    CHECK(CanonicalLevelName(LogLevel::Info) == "Info");
    CHECK(CanonicalLevelName(LogLevel::Warn) == "Warn");
    CHECK(CanonicalLevelName(LogLevel::Error) == "Error");
    CHECK(CanonicalLevelName(LogLevel::Fatal) == "Fatal");
    CHECK(CanonicalLevelName(LogLevel::Unknown) == "Unknown");
}

TEST_CASE("ResolveLevel honours per-column alias overrides", "[log_level]")
{
    const std::vector<std::pair<std::string, std::string>> overrides{
        {"PANIC", "Fatal"},
        // Bogus canonical name -- ignored.
        {"WAT", "NotARealLevel"},
        // Re-map a built-in alias to a different level.
        {"warn", "Error"},
        // Numeric mapping: numbers are deliberately *not* in the
        // built-in table, but `levelMapping` makes them work when the
        // producer emits them as JSON strings.
        {"30", "Info"},
        {"40", "Warn"},
    };

    CHECK(ResolveLevel("PANIC", overrides) == LogLevel::Fatal);
    // Override takes precedence over built-in.
    CHECK(ResolveLevel("warn", overrides) == LogLevel::Error);
    // Invalid override falls through to built-in (no match here).
    CHECK_FALSE(ResolveLevel("WAT", overrides).has_value());
    // Built-in alias still works for entries not in overrides.
    CHECK(ResolveLevel("info", overrides) == LogLevel::Info);
    // Numeric strings only resolve through the per-column mapping.
    CHECK(ResolveLevel("30", overrides) == LogLevel::Info);
    CHECK(ResolveLevel("40", overrides) == LogLevel::Warn);
    CHECK_FALSE(ResolveLevel("50", overrides).has_value());
    // New short-form built-ins still resolve when no override matches.
    CHECK(ResolveLevel("inf", overrides) == LogLevel::Info);
    CHECK(ResolveLevel("i", overrides) == LogLevel::Info);
    CHECK(ResolveLevel("ftl", overrides) == LogLevel::Fatal);
}

// -----------------------------------------------------------------------------
// Schema back/forward-compat tests pinning `error_on_unknown_keys=false`
// (configured in `log_configuration_glaze_opts.hpp`). Without that
// option every pre-widening session JSON (using the old `"locator"`
// field) would throw on load.
// -----------------------------------------------------------------------------

TEST_CASE(
    "Legacy `\"locator\"` session JSON loads with columns and filters intact (source rebind drops to nullopt)",
    "[log_configuration][session][source][compat]"
)
{
    const TestLogConfiguration legacyFile("test_log_configuration_legacy_locator.json");

    // Pre-widening shape: `source.locator` (single string) instead of
    // the current `locators` array. Columns / filters / sort match
    // the new schema; only the source's inner field is renamed.
    constexpr std::string_view LEGACY_JSON = R"({
   "columns": [
      {
         "header": "timestamp",
         "keys": ["timestamp"],
         "printFormat": "%F %H:%M:%S",
         "type": "time",
         "parseFormats": ["%FT%T%Ez", "%F %T%Ez", "%FT%T", "%F %T"]
      },
      {
         "header": "msg",
         "keys": ["msg"],
         "printFormat": "{}",
         "type": "string",
         "parseFormats": []
      }
   ],
   "filters": [
      {
         "type": "string",
         "row": 1,
         "filterString": "boot",
         "matchType": "contains"
      }
   ],
   "sort": {
      "columnIndex": 0,
      "descending": true
   },
   "source": {
      "kind": "file",
      "locator": "C:/logs/legacy.json"
   }
})";

    {
        std::ofstream stream(legacyFile.GetFilePath(), std::ios::binary);
        REQUIRE(stream.is_open());
        stream << LEGACY_JSON;
    }

    LogConfigurationManager manager;
    REQUIRE_NOTHROW(manager.Load(legacyFile.GetFilePath()));

    // Columns / filters / sort survive despite the renamed source
    // field (without the `error_on_unknown_keys=false` opt-in,
    // this would throw and lose the entire session).
    REQUIRE(manager.Configuration().columns.size() == 2);
    CHECK(manager.Configuration().columns[0].header == "timestamp");
    CHECK(manager.Configuration().columns[0].type == LogConfiguration::Type::Time);
    CHECK(manager.Configuration().columns[1].header == "msg");

    REQUIRE(manager.Configuration().filters.size() == 1);
    CHECK(manager.Configuration().filters[0].row == 1);
    REQUIRE(manager.Configuration().filters[0].filterString.has_value());
    CHECK(*manager.Configuration().filters[0].filterString == "boot");

    CHECK(manager.Configuration().sort.columnIndex == 0);
    CHECK(manager.Configuration().sort.descending);

    // The legacy `locator` field is unknown and silently dropped;
    // `source` ends up with empty locators. `HasLocators` correctly
    // reports "not actionable" so rebind falls through.
    CHECK_FALSE(loglib::HasLocators(manager.Configuration().source));
}

TEST_CASE(
    "Unknown top-level field in a session JSON does not abort the load", "[log_configuration][session][source][compat]"
)
{
    const TestLogConfiguration futureFile("test_log_configuration_future_field.json");

    // Future-build JSON with an unknown top-level field (`hooks`).
    // Current build must read what it understands and ignore the rest.
    constexpr std::string_view FUTURE_JSON = R"({
   "columns": [
      {
         "header": "msg",
         "keys": ["msg"],
         "printFormat": "{}",
         "type": "string",
         "parseFormats": []
      }
   ],
   "filters": [],
   "sort": {
      "columnIndex": -1,
      "descending": false
   },
   "source": {
      "kind": "file",
      "locators": ["C:/logs/example.json"]
   },
   "hooks": {
      "onLoad": "future-thing",
      "onSave": "another-future-thing"
   }
})";

    {
        std::ofstream stream(futureFile.GetFilePath(), std::ios::binary);
        REQUIRE(stream.is_open());
        stream << FUTURE_JSON;
    }

    LogConfigurationManager manager;
    REQUIRE_NOTHROW(manager.Load(futureFile.GetFilePath()));

    REQUIRE(manager.Configuration().columns.size() == 1);
    CHECK(manager.Configuration().columns[0].header == "msg");
    REQUIRE(loglib::HasLocators(manager.Configuration().source));
    CHECK(manager.Configuration().source->locators.front() == "C:/logs/example.json");
}

TEST_CASE("Empty `locators` round-trips through Save / Load as an empty array", "[log_configuration][session][source]")
{
    LogConfiguration original;
    original.source = LogConfiguration::Source{.kind = LogConfiguration::Source::Kind::File, .locators = {}};

    std::string json;
    const auto writeError = glz::write_json(original, json);
    REQUIRE_FALSE(writeError);

    LogConfiguration loaded;
    const auto readError = glz::read_json(loaded, json);
    REQUIRE_FALSE(readError);

    REQUIRE(loaded.source.has_value());
    CHECK(loaded.source->kind == LogConfiguration::Source::Kind::File);
    CHECK(loaded.source->locators.empty());

    // `HasLocators` is the canonical "is the source actionable"
    // gate; `has_value()` alone misses the empty-locator case.
    CHECK_FALSE(loglib::HasLocators(loaded.source));
}

TEST_CASE(
    "Pretty-write emits `locators` as a JSON array (wire-format snapshot)", "[log_configuration][session][source]"
)
{
    LogConfiguration original;
    original.source = LogConfiguration::Source{
        .kind = LogConfiguration::Source::Kind::File, .locators = {"C:/logs/a.json", "C:/logs/b.json"}
    };

    const TestLogConfiguration file("test_log_configuration_pretty_locators.json");
    LogConfigurationManager::Save(original, file.GetFilePath(), SaveScope::Full);

    std::ifstream readBack(file.GetFilePath());
    REQUIRE(readBack.is_open());
    const std::string raw((std::istreambuf_iterator<char>(readBack)), std::istreambuf_iterator<char>());

    // Pin the exact wire-format key; a future rename here would
    // break every user's recents store.
    CHECK(raw.contains("\"locators\""));
    // Word-boundary guard: `locators` contains `locator`, so look
    // for the colon-suffixed legacy form rather than a substring.
    CHECK_FALSE(raw.contains("\"locator\":"));
    CHECK_FALSE(raw.contains("\"locator\" :"));
    CHECK(raw.contains("\"C:/logs/a.json\""));
    CHECK(raw.contains("\"C:/logs/b.json\""));
}

TEST_CASE("HasLocators predicate exhaustively handles every Source state", "[log_configuration][session][source]")
{
    // nullopt: no source bound.
    CHECK_FALSE(loglib::HasLocators(std::nullopt));

    // Present but empty: not actionable. `has_value()` alone would
    // erroneously claim a binding here.
    LogConfiguration::Source emptyLocators;
    emptyLocators.kind = LogConfiguration::Source::Kind::File;
    CHECK_FALSE(loglib::HasLocators(std::optional<LogConfiguration::Source>{emptyLocators}));

    LogConfiguration::Source oneLocator;
    oneLocator.kind = LogConfiguration::Source::Kind::File;
    oneLocator.locators.emplace_back("C:/x.json");
    CHECK(loglib::HasLocators(std::optional<LogConfiguration::Source>{oneLocator}));
}

TEST_CASE("LogConfiguration::anchors round-trips through Save/Load", "[log_configuration][session][anchors]")
{
    // Two anchors in different palette slots; one carries a locator
    // (multi-file session), the other doesn't. The list is preserved
    // verbatim across a full Save / Load cycle.
    const TestLogConfiguration testConfiguration;
    {
        LogConfiguration written;
        written.anchors.push_back(
            LogConfiguration::AnchorEntry{
                .locator = "",
                .lineId = 17,
                .colorIndex = 0,
            }
        );
        written.anchors.push_back(
            LogConfiguration::AnchorEntry{
                .locator = "c:/logs/two.json",
                .lineId = 42,
                .colorIndex = 5,
            }
        );
        LogConfigurationManager::Save(written, testConfiguration.GetFilePath(), SaveScope::Full);
    }

    LogConfigurationManager manager;
    manager.Load(testConfiguration.GetFilePath());
    const auto &anchors = manager.Configuration().anchors;
    REQUIRE(anchors.size() == 2);
    CHECK(anchors[0].locator.empty());
    CHECK(anchors[0].lineId == 17u);
    CHECK(anchors[0].colorIndex == 0u);
    CHECK(anchors[1].locator == "c:/logs/two.json");
    CHECK(anchors[1].lineId == 42u);
    CHECK(anchors[1].colorIndex == 5u);
}

TEST_CASE(
    "LogConfiguration without anchors loads as empty list", "[log_configuration][session][anchors][forward_compat]"
)
{
    // Pre-feature session JSON: no `anchors` key at all. Must load
    // cleanly with an empty anchor vector so existing users' saved
    // sessions are not invalidated by this release.
    constexpr std::string_view JSON = R"({
        "columns": [],
        "filters": [],
        "sort": { "columnIndex": -1, "descending": false }
    })";

    LogConfigurationManager manager;
    manager.LoadFromString(JSON);
    CHECK(manager.Configuration().anchors.empty());
}

TEST_CASE("LogConfiguration::AnchorEntry serialises with stable wire keys", "[log_configuration][session][anchors]")
{
    // Wire-format snapshot: an entry rename would surface here as a
    // missing key, not as a silent shape change.
    LogConfiguration original;
    original.anchors.push_back(
        LogConfiguration::AnchorEntry{
            .locator = "c:/logs/app.jsonl",
            .lineId = 123,
            .colorIndex = 3,
        }
    );

    std::string json;
    const auto writeError = glz::write_json(original, json);
    REQUIRE_FALSE(writeError);
    CHECK(json.contains("\"anchors\""));
    CHECK(json.contains("\"locator\""));
    CHECK(json.contains("\"lineId\""));
    CHECK(json.contains("\"colorIndex\""));

    LogConfiguration loaded;
    const auto readError = glz::read_json(loaded, json);
    REQUIRE_FALSE(readError);
    REQUIRE(loaded.anchors.size() == 1);
    CHECK(loaded.anchors[0].locator == "c:/logs/app.jsonl");
    CHECK(loaded.anchors[0].lineId == 123u);
    CHECK(loaded.anchors[0].colorIndex == 3u);
}
