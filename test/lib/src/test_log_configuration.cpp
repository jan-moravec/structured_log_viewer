#include "common.hpp"

#include <loglib/internal/log_configuration_glaze_meta.hpp>
#include <loglib/key_index.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_data.hpp>
#include <loglib/log_line.hpp>

#include <catch2/catch_all.hpp>
#include <glaze/glaze.hpp>

#include <filesystem>

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
        .header = "test", .keys = {"test"}, .printFormat = "{}", .type = LogConfiguration::Type::any, .parseFormats = {}
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
    "AppendKeys recognises common timestamp aliases as Type::time and ignores bare 't'",
    "[LogConfigurationManager][timestamp_keys][regression]"
)
{
    // Regression: the bare "t" used to land here (one-letter
    // columns named `t` were silently auto-promoted to a date
    // column). Widen-and-prune the canonical list at the same
    // time: the well-known JSON timestamp aliases (`@timestamp`,
    // `datetime`, `created_at`, `ts`) now route through the same
    // first-column promotion path.
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

    checkType("timestamp", LogConfiguration::Type::time);
    checkType("time", LogConfiguration::Type::time);
    checkType("ts", LogConfiguration::Type::time);
    checkType("@timestamp", LogConfiguration::Type::time);
    checkType("datetime", LogConfiguration::Type::time);
    checkType("created_at", LogConfiguration::Type::time);

    // `t` and `tag` are NOT timestamp keys: they keep the
    // free-text-friendly `Type::unknown` and are not promoted.
    checkType("t", LogConfiguration::Type::unknown);
    checkType("tag", LogConfiguration::Type::unknown);
}

TEST_CASE("Update with mixed keys organizes timestamp first", "[LogConfigurationManager]")
{
    const TestLogConfiguration testLogConfiguration;

    LogConfiguration logConfiguration;
    logConfiguration.columns.push_back(
        {.header = "regular",
         .keys = {"regular"},
         .printFormat = "{}",
         .type = LogConfiguration::Type::any,
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
    CHECK(manager.Configuration().columns[0].type == LogConfiguration::Type::time);

    CHECK(manager.Configuration().columns[1].header == "regular");
    CHECK(manager.Configuration().columns[2].header == "newKey");

    CHECK(manager.Configuration().columns[0].printFormat == "%F %H:%M:%S");
    REQUIRE(manager.Configuration().columns[0].parseFormats.size() == 4);
    CHECK(manager.Configuration().columns[0].parseFormats[0] == "%FT%T%Ez");
}

// `IsKeyInAnyColumn` cache-invalidation tests: every mutating path
// (`Load`, `Update`, `AppendKeys`) must flip the cache-stale flag so the
// next query reflects the new column set.

TEST_CASE(
    "Cache: AppendKeys after a query sees the freshly appended key", "[LogConfigurationManager][cache_invalidation]"
)
{
    LogConfigurationManager manager;

    // First AppendKeys builds the cache from an empty column set; it must
    // stay coherent across the column appended below.
    manager.AppendKeys({"alpha"});
    REQUIRE(manager.Configuration().columns.size() == 1);
    CHECK(manager.Configuration().columns[0].header == "alpha");

    // Existing-key check must hit the cache while the new key is added,
    // and the duplicate "alpha" inside the call must not double-add.
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

    // Seed via AppendKeys so the cache is primed across the next mutator.
    manager.AppendKeys({"regular"});
    REQUIRE(manager.Configuration().columns.size() == 1);

    // Update must skip "regular", add "timestamp" as Type::time, and place
    // it at position 0.
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
    CHECK(manager.Configuration().columns[0].type == LogConfiguration::Type::time);
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
             .type = LogConfiguration::Type::any,
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
             .type = LogConfiguration::Type::any,
             .parseFormats = {}}
        );
        secondConfigOnDisk.Write(logConfiguration);
    }

    LogConfigurationManager manager;

    // Prime the cache with a key that is NOT in the on-disk configuration;
    // Load must replace the cache so the post-Load AppendKeys re-adds it.
    manager.AppendKeys({"stale_key"});
    REQUIRE(manager.Configuration().columns.size() == 1);
    CHECK(manager.Configuration().columns[0].header == "stale_key");

    manager.Load(firstConfigOnDisk.GetFilePath());
    REQUIRE(manager.Configuration().columns.size() == 1);
    CHECK(manager.Configuration().columns[0].header == "loaded_key");

    // `stale_key` is gone post-Load; AppendKeys must re-add it.
    manager.AppendKeys({"stale_key", "loaded_key"});
    REQUIRE(manager.Configuration().columns.size() == 2);
    CHECK(manager.Configuration().columns[0].header == "loaded_key");
    CHECK(manager.Configuration().columns[1].header == "stale_key");

    // A second Load again replaces the column set / cache wholesale.
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
    // Re-run the "mixed keys organizes timestamp first" shape with the
    // cached path; regression boundary against the old free-function walk.
    const TestLogConfiguration testLogConfiguration;
    LogConfiguration logConfiguration;
    logConfiguration.columns.push_back(
        {.header = "regular",
         .keys = {"regular"},
         .printFormat = "{}",
         .type = LogConfiguration::Type::any,
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
    // The cache mirrors `column.keys`, not `column.header`. With diverging
    // header/keys, AppendKeys must skip keys already listed under any
    // column's `keys`.
    const TestLogConfiguration testLogConfiguration;
    LogConfiguration logConfiguration;
    logConfiguration.columns.push_back(
        {.header = "display",
         .keys = {"raw_key", "alias"},
         .printFormat = "{}",
         .type = LogConfiguration::Type::any,
         .parseFormats = {}}
    );
    testLogConfiguration.Write(logConfiguration);

    LogConfigurationManager manager;
    manager.Load(testLogConfiguration.GetFilePath());

    manager.AppendKeys({"display", "raw_key", "alias", "fresh"});

    // Existing column stays; only "display" and "fresh" are added
    // ("raw_key"/"alias" hit the keys cache).
    REQUIRE(manager.Configuration().columns.size() == 3);
    CHECK(manager.Configuration().columns[0].header == "display");
    CHECK(manager.Configuration().columns[1].header == "display"); // header reused; keys differ
    CHECK(manager.Configuration().columns[2].header == "fresh");
    CHECK(manager.Configuration().columns[1].keys == std::vector<std::string>{"display"});
    CHECK(manager.Configuration().columns[2].keys == std::vector<std::string>{"fresh"});
}

TEST_CASE("Save and load configuration with Type::enumeration column", "[log_configuration][enum]")
{
    const TestLogConfiguration testLogConfiguration("test_log_configuration_enum.json");

    LogConfiguration logConfiguration;
    logConfiguration.columns.push_back(
        {.header = "Level",
         .keys = {"level", "severity"},
         .printFormat = "{}",
         .type = LogConfiguration::Type::enumeration,
         .parseFormats = {}}
    );
    testLogConfiguration.Write(logConfiguration);

    LogConfigurationManager manager;
    manager.Load(testLogConfiguration.GetFilePath());

    REQUIRE(manager.Configuration().columns.size() == 1);
    CHECK(manager.Configuration().columns[0].header == "Level");
    CHECK(manager.Configuration().columns[0].type == LogConfiguration::Type::enumeration);
    CHECK(manager.Configuration().columns[0].keys == std::vector<std::string>{"level", "severity"});
}

TEST_CASE(
    "Newly-discovered keys default to Type::unknown so the auto-detector scans them",
    "[log_configuration][type_unknown]"
)
{
    // The `Type::unknown` -> terminal-type transition replaces the
    // old `mAutoDiscoveredCanonicalKeys` side-channel: provenance is
    // carried by the column type itself, both in memory and on disk.

    SECTION("AppendKeys assigns Type::unknown to fresh keys")
    {
        LogConfigurationManager manager;
        manager.AppendKeys({"level"});
        REQUIRE(manager.Configuration().columns.size() == 1);
        CHECK(manager.Configuration().columns[0].type == LogConfiguration::Type::unknown);
    }

    SECTION("Update assigns Type::unknown to every freshly-added non-time key")
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
            CHECK(column.type == LogConfiguration::Type::unknown);
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
             .type = LogConfiguration::Type::any,
             .parseFormats = {}}
        );
        cfg.columns.push_back(
            {.header = "service",
             .keys = {"service"},
             .printFormat = "{}",
             .type = LogConfiguration::Type::string,
             .parseFormats = {}}
        );
        testCfg.Write(cfg);

        LogConfigurationManager manager;
        manager.Load(testCfg.GetFilePath());

        REQUIRE(manager.Configuration().columns.size() == 2);
        CHECK(manager.Configuration().columns[0].type == LogConfiguration::Type::any);
        CHECK(manager.Configuration().columns[1].type == LogConfiguration::Type::string);
    }

    SECTION("Post-Load AppendKeys still assigns Type::unknown to genuinely-new keys")
    {
        const TestLogConfiguration testCfg;
        LogConfiguration cfg;
        cfg.columns.push_back(
            {.header = "level",
             .keys = {"level"},
             .printFormat = "{}",
             .type = LogConfiguration::Type::any,
             .parseFormats = {}}
        );
        testCfg.Write(cfg);

        LogConfigurationManager manager;
        manager.Load(testCfg.GetFilePath());

        manager.AppendKeys({"freshly_streamed"});
        REQUIRE(manager.Configuration().columns.size() == 2);
        // Loaded `level` stays terminal; the freshly-streamed key
        // becomes a candidate.
        CHECK(manager.Configuration().columns[0].type == LogConfiguration::Type::any);
        CHECK(manager.Configuration().columns[1].type == LogConfiguration::Type::unknown);
    }
}

TEST_CASE("Round-trip preserves every LogConfiguration::Type variant", "[log_configuration][type_round_trip]")
{
    // Glaze meta string mapping: the wire format uses human-readable
    // enum names that match the C++ enumerator one-for-one. `double`
    // is a reserved keyword in C++, so the enumerator and the JSON
    // string both spell it `floating` — no translation table.
    using Type = LogConfiguration::Type;
    const std::vector<Type> variants = {
        Type::unknown,
        Type::any,
        Type::string,
        Type::integer,
        Type::floating,
        Type::number,
        Type::time,
        Type::enumeration,
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

    // Sanity-check the wire format. Every variant uses its C++
    // identifier verbatim on the wire; the legacy `"double"` spelling
    // must never appear.
    CHECK(json.find("\"unknown\"") != std::string::npos);
    CHECK(json.find("\"any\"") != std::string::npos);
    CHECK(json.find("\"integer\"") != std::string::npos);
    CHECK(json.find("\"floating\"") != std::string::npos);
    CHECK(json.find("\"double\"") == std::string::npos);
    CHECK(json.find("\"number\"") != std::string::npos);
    CHECK(json.find("\"enumeration\"") != std::string::npos);

    LogConfiguration loaded;
    const auto readError = glz::read_json(loaded, json);
    REQUIRE_FALSE(readError);

    REQUIRE(loaded.columns.size() == variants.size());
    for (size_t i = 0; i < variants.size(); ++i)
    {
        CHECK(loaded.columns[i].type == variants[i]);
    }
}

TEST_CASE("Round-trip LogFilter with Type::enumeration and filterValues", "[log_configuration][enum]")
{
    LogConfiguration original;
    LogConfiguration::LogFilter filter;
    filter.type = LogConfiguration::LogFilter::Type::enumeration;
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
    CHECK(loaded.filters[0].type == LogConfiguration::LogFilter::Type::enumeration);
    CHECK(loaded.filters[0].row == 2);
    CHECK(loaded.filters[0].filterValues == std::vector<std::string>{"info", "warn", "error"});
}
