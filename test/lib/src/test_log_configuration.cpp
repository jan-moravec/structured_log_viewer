#include "common.hpp"

#include <loglib/key_index.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_data.hpp>
#include <loglib/log_file.hpp>
#include <loglib/log_line.hpp>

#include <catch2/catch_all.hpp>
#include <glaze/glaze.hpp>

#include <filesystem>
#include <fstream>

using namespace loglib;

TEST_CASE("Save and load empty configuration", "[LogConfigurationManager]")
{
    TestLogConfiguration testConfiguration;

    {
        LogConfigurationManager manager;
        manager.Save(testConfiguration.GetFilePath());
    }

    // Load configuration from the file
    {
        LogConfigurationManager manager;
        manager.Load(testConfiguration.GetFilePath());

        // Verify loaded configuration is empty
        CHECK(manager.Configuration().columns.size() == 0);
        CHECK(manager.Configuration().filters.size() == 0);
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
    TestLogConfiguration testConfiguration;
    LogConfigurationManager manager;

    CHECK_THROWS_AS(manager.Load(testConfiguration.GetFilePath()), std::runtime_error);
}

TEST_CASE("Update with empty LogData should not modify configuration", "[LogConfigurationManager]")
{
    TestLogConfiguration testLogConfiguration;

    LogConfiguration logConfiguration;
    const LogConfiguration::Column defaultColumn = {"test", {"test"}, "{}", LogConfiguration::Type::any, {}};
    logConfiguration.columns.push_back(defaultColumn);
    testLogConfiguration.Write(logConfiguration);

    LogConfigurationManager manager;
    manager.Load(testLogConfiguration.GetFilePath());

    const size_t initialColumnCount = manager.Configuration().columns.size();

    LogData emptyLogData;
    manager.Update(emptyLogData);

    CHECK(manager.Configuration().columns.size() == initialColumnCount);
    CHECK(manager.Configuration().columns[0].header == defaultColumn.header);
    CHECK(manager.Configuration().columns[0].keys == defaultColumn.keys);
    CHECK(manager.Configuration().columns[0].parseFormats == defaultColumn.parseFormats);
    CHECK(manager.Configuration().columns[0].printFormat == defaultColumn.printFormat);
    CHECK(manager.Configuration().columns[0].type == defaultColumn.type);
}

TEST_CASE("Update with mixed keys organizes timestamp first", "[LogConfigurationManager]")
{
    TestLogConfiguration testLogConfiguration;

    LogConfiguration logConfiguration;
    logConfiguration.columns.push_back({"regular", {"regular"}, "{}", LogConfiguration::Type::any, {}});
    testLogConfiguration.Write(logConfiguration);

    LogConfigurationManager manager;
    manager.Load(testLogConfiguration.GetFilePath());

    TestLogFile testLogFile;
    auto source = testLogFile.CreateFileLineSource();
    KeyIndex testKeys;
    std::vector<LogLine> testLines;
    testLines.emplace_back(LogMap{{"regular", std::string("value")}}, testKeys, *source, 0);
    testLines.emplace_back(LogMap{{"newKey", std::string("test")}}, testKeys, *source, 0);
    testLines.emplace_back(LogMap{{"timestamp", std::string("2023-01-01T12:00:00Z")}}, testKeys, *source, 0);

    LogData logData(std::move(source), std::move(testLines), std::move(testKeys));

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
    TestLogFile testLogFile;
    auto source = testLogFile.CreateFileLineSource();
    KeyIndex testKeys;
    std::vector<LogLine> testLines;
    testLines.emplace_back(LogMap{{"regular", std::string("value")}}, testKeys, *source, 0);
    testLines.emplace_back(LogMap{{"timestamp", std::string("2023-01-01T12:00:00Z")}}, testKeys, *source, 0);
    LogData logData(std::move(source), std::move(testLines), std::move(testKeys));

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
    TestLogConfiguration firstConfigOnDisk("test_config_first.json");
    {
        LogConfiguration logConfiguration;
        logConfiguration.columns.push_back({"loaded_key", {"loaded_key"}, "{}", LogConfiguration::Type::any, {}});
        firstConfigOnDisk.Write(logConfiguration);
    }
    TestLogConfiguration secondConfigOnDisk("test_config_second.json");
    {
        LogConfiguration logConfiguration;
        logConfiguration.columns.push_back({"other_key", {"other_key"}, "{}", LogConfiguration::Type::any, {}});
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
    TestLogConfiguration testLogConfiguration;
    LogConfiguration logConfiguration;
    logConfiguration.columns.push_back({"regular", {"regular"}, "{}", LogConfiguration::Type::any, {}});
    testLogConfiguration.Write(logConfiguration);

    LogConfigurationManager manager;
    manager.Load(testLogConfiguration.GetFilePath());

    TestLogFile testLogFile;
    auto source = testLogFile.CreateFileLineSource();
    KeyIndex testKeys;
    std::vector<LogLine> testLines;
    testLines.emplace_back(LogMap{{"regular", std::string("value")}}, testKeys, *source, 0);
    testLines.emplace_back(LogMap{{"newKey", std::string("test")}}, testKeys, *source, 0);
    testLines.emplace_back(LogMap{{"timestamp", std::string("2023-01-01T12:00:00Z")}}, testKeys, *source, 0);
    LogData logData(std::move(source), std::move(testLines), std::move(testKeys));

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
    TestLogConfiguration testLogConfiguration;
    LogConfiguration logConfiguration;
    logConfiguration.columns.push_back({"display", {"raw_key", "alias"}, "{}", LogConfiguration::Type::any, {}});
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
