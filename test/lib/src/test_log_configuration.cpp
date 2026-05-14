#include "common.hpp"

#include <loglib/internal/log_configuration_glaze_meta.hpp>
#include <loglib/key_index.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_data.hpp>
#include <loglib/log_line.hpp>

#include <catch2/catch_all.hpp>
#include <glaze/glaze.hpp>

#include <filesystem>
#include <fstream>
#include <string_view>

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

    checkType("t", LogConfiguration::Type::Unknown);
    checkType("tag", LogConfiguration::Type::Unknown);
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
    "Newly-discovered keys default to Type::Unknown so the auto-detector scans them",
    "[log_configuration][type_unknown]"
)
{
    // Provenance is carried by the column type itself, both in memory
    // and on disk; `Type::Unknown` marks an auto-detector candidate.

    SECTION("AppendKeys assigns Type::Unknown to fresh keys")
    {
        LogConfigurationManager manager;
        manager.AppendKeys({"level"});
        REQUIRE(manager.Configuration().columns.size() == 1);
        CHECK(manager.Configuration().columns[0].type == LogConfiguration::Type::Unknown);
    }

    SECTION("Update assigns Type::Unknown to every freshly-added non-time key")
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
            CHECK(column.type == LogConfiguration::Type::Unknown);
        }
    }

    SECTION("Loaded columns keep their stored Type and are not rewritten to unknown")
    {
        const TestLogConfiguration testCfg;
        LogConfiguration cfg;
        cfg.columns.push_back(
            {.header = "level",
             .keys = {"level"},
             .printFormat = "{}",
             .type = LogConfiguration::Type::Any,
             .parseFormats = {}}
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
        CHECK(manager.Configuration().columns[1].type == LogConfiguration::Type::String);
    }

    SECTION("Post-Load AppendKeys still assigns Type::Unknown to genuinely-new keys")
    {
        const TestLogConfiguration testCfg;
        LogConfiguration cfg;
        cfg.columns.push_back(
            {.header = "level",
             .keys = {"level"},
             .printFormat = "{}",
             .type = LogConfiguration::Type::Any,
             .parseFormats = {}}
        );
        testCfg.Write(cfg);

        LogConfigurationManager manager;
        manager.Load(testCfg.GetFilePath());

        manager.AppendKeys({"freshly_streamed"});
        REQUIRE(manager.Configuration().columns.size() == 2);
        CHECK(manager.Configuration().columns[0].type == LogConfiguration::Type::Any);
        CHECK(manager.Configuration().columns[1].type == LogConfiguration::Type::Unknown);
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
        Type::Unknown,
        Type::Any,
        Type::String,
        Type::Boolean,
        Type::Integer,
        Type::Floating,
        Type::Number,
        Type::Time,
        Type::Enumeration,
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

    CHECK(json.contains("\"unknown\""));
    CHECK(json.contains("\"any\""));
    CHECK(json.contains("\"boolean\""));
    CHECK(json.contains("\"integer\""));
    CHECK(json.contains("\"floating\""));
    CHECK_FALSE(json.contains("\"double\""));
    CHECK(json.contains("\"number\""));
    CHECK(json.contains("\"enumeration\""));

    LogConfiguration loaded;
    const auto readError = glz::read_json(loaded, json);
    REQUIRE_FALSE(readError);

    REQUIRE(loaded.columns.size() == variants.size());
    for (size_t i = 0; i < variants.size(); ++i)
    {
        CHECK(loaded.columns[i].type == variants[i]);
    }
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
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(*loaded.filters[0].filterMinValue == Catch::Approx(-2.5));
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(*loaded.filters[0].filterMaxValue == Catch::Approx(17.25));

    CHECK_FALSE(loaded.filters[1].filterMinValue.has_value());
    REQUIRE(loaded.filters[1].filterMaxValue.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(*loaded.filters[1].filterMaxValue == Catch::Approx(100.0));

    REQUIRE(loaded.filters[2].filterMinValue.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
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
            {"header":"a","keys":["a"],"printFormat":"{}","type":"unknown","parseFormats":[]},
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
    REQUIRE(loaded.columns.size() == 8);
    CHECK(loaded.columns[0].type == Type::Unknown);
    CHECK(loaded.columns[1].type == Type::Any);
    CHECK(loaded.columns[2].type == Type::String);
    CHECK(loaded.columns[3].type == Type::Integer);
    CHECK(loaded.columns[4].type == Type::Floating);
    CHECK(loaded.columns[5].type == Type::Number);
    CHECK(loaded.columns[6].type == Type::Time);
    CHECK(loaded.columns[7].type == Type::Enumeration);

    using FilterType = LogConfiguration::LogFilter::Type;
    using Match = LogConfiguration::LogFilter::Match;
    REQUIRE(loaded.filters.size() == 6);
    CHECK(loaded.filters[0].type == FilterType::String);
    // The `REQUIRE(has_value())` guards above are not modelled by
    // `bugprone-unchecked-optional-access` (only `if`/`DCHECK`/`ASSERT_TRUE`
    // are), so the `operator*` accesses below are false positives.
    REQUIRE(loaded.filters[0].matchType.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(*loaded.filters[0].matchType == Match::Exactly);
    REQUIRE(loaded.filters[1].matchType.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(*loaded.filters[1].matchType == Match::Contains);
    REQUIRE(loaded.filters[2].matchType.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(*loaded.filters[2].matchType == Match::RegularExpression);
    REQUIRE(loaded.filters[3].matchType.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(*loaded.filters[3].matchType == Match::Wildcard);
    CHECK(loaded.filters[4].type == FilterType::Time);
    CHECK(loaded.filters[5].type == FilterType::Enumeration);

    // Re-serialise and confirm the wire format keeps the original keys.
    std::string roundTripJson;
    const auto writeError = glz::write_json(loaded, roundTripJson);
    REQUIRE_FALSE(writeError);
    CHECK(roundTripJson.contains("\"unknown\""));
    CHECK(roundTripJson.contains("\"enumeration\""));
    CHECK(roundTripJson.contains("\"regularExpression\""));
    CHECK_FALSE(roundTripJson.contains("\"Unknown\""));
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
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(*loaded.filters[0].filterMinValue == Catch::Approx(1.5));
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    CHECK(*loaded.filters[0].filterMaxValue == Catch::Approx(5.0));

    CHECK(loaded.filters[1].type == FilterType::Boolean);
    CHECK(loaded.filters[1].filterValues == std::vector<std::string>{"true", "false"});
}

TEST_CASE("Column::visible round-trips through Save/Load", "[LogConfigurationManager][column_visibility]")
{
    // The hidden-column flag must survive Save / Load.
    const TestLogConfiguration testConfiguration;

    {
        LogConfiguration configuration;
        configuration.columns.push_back(LogConfiguration::Column{
            .header = "shown",
            .keys = {"shown"},
            .printFormat = "{}",
            .type = LogConfiguration::Type::String,
            .parseFormats = {},
            .visible = true,
        });
        configuration.columns.push_back(LogConfiguration::Column{
            .header = "hidden",
            .keys = {"hidden"},
            .printFormat = "{}",
            .type = LogConfiguration::Type::String,
            .parseFormats = {},
            .visible = false,
        });
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
            {"header":"a","keys":["a"],"printFormat":"{}","type":"unknown","parseFormats":[]}
        ],
        "filters": []
    })";

    LogConfiguration loaded;
    const auto readError = glz::read_json(loaded, LEGACY_JSON);
    REQUIRE_FALSE(readError);
    REQUIRE(loaded.columns.size() == 1);
    CHECK(loaded.columns[0].visible);
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
            configuration.filters.push_back(LogConfiguration::LogFilter{
                .type = LogConfiguration::LogFilter::Type::String,
                .row = row,
                .filterString = std::string{"x"},
                .matchType = LogConfiguration::LogFilter::Match::Contains,
                .filterBegin = std::nullopt,
                .filterEnd = std::nullopt,
                .filterMinValue = std::nullopt,
                .filterMaxValue = std::nullopt,
                .filterValues = {},
            });
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
        configuration.filters.push_back(LogConfiguration::LogFilter{
            .type = LogConfiguration::LogFilter::Type::String,
            .row = 0,
            .filterString = std::string{"x"},
            .matchType = LogConfiguration::LogFilter::Match::Contains,
            .filterBegin = std::nullopt,
            .filterEnd = std::nullopt,
            .filterMinValue = std::nullopt,
            .filterMaxValue = std::nullopt,
            .filterValues = {},
        });
        configuration.filters.push_back(LogConfiguration::LogFilter{
            .type = LogConfiguration::LogFilter::Type::String,
            .row = 1,
            .filterString = std::string{"y"},
            .matchType = LogConfiguration::LogFilter::Match::Contains,
            .filterBegin = std::nullopt,
            .filterEnd = std::nullopt,
            .filterMinValue = std::nullopt,
            .filterMaxValue = std::nullopt,
            .filterValues = {},
        });
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
        good.columns.push_back(LogConfiguration::Column{
            .header = "good_a",
            .keys = {"a"},
            .printFormat = "{}",
            .type = LogConfiguration::Type::String,
            .parseFormats = {},
            .visible = true,
        });
        good.columns.push_back(LogConfiguration::Column{
            .header = "good_b",
            .keys = {"b"},
            .printFormat = "{}",
            .type = LogConfiguration::Type::Integer,
            .parseFormats = {},
            .visible = false,
        });
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
