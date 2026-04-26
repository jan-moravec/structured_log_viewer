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
    // Setup test configuration
    TestLogConfiguration testLogConfiguration;

    LogConfiguration logConfiguration;
    const LogConfiguration::Column defaultColumn = {"test", {"test"}, "{}", LogConfiguration::Type::any, {}};
    logConfiguration.columns.push_back(defaultColumn);
    testLogConfiguration.Write(logConfiguration);

    // Create a configuration and load the predefined configuration
    LogConfigurationManager manager;
    manager.Load(testLogConfiguration.GetFilePath());

    // Initial configuration state
    const size_t initialColumnCount = manager.Configuration().columns.size();

    // Create empty LogData
    LogData emptyLogData;

    // Update with empty LogData
    manager.Update(emptyLogData);

    // Verify configuration hasn't changed
    CHECK(manager.Configuration().columns.size() == initialColumnCount);
    CHECK(manager.Configuration().columns[0].header == defaultColumn.header);
    CHECK(manager.Configuration().columns[0].keys == defaultColumn.keys);
    CHECK(manager.Configuration().columns[0].parseFormats == defaultColumn.parseFormats);
    CHECK(manager.Configuration().columns[0].printFormat == defaultColumn.printFormat);
    CHECK(manager.Configuration().columns[0].type == defaultColumn.type);
}

TEST_CASE("Update with mixed keys organizes timestamp first", "[LogConfigurationManager]")
{
    // Setup test configuration with an initial regular column
    TestLogConfiguration testLogConfiguration;

    LogConfiguration logConfiguration;
    logConfiguration.columns.push_back({"regular", {"regular"}, "{}", LogConfiguration::Type::any, {}});
    testLogConfiguration.Write(logConfiguration);

    // Load the configuration
    LogConfigurationManager manager;
    manager.Load(testLogConfiguration.GetFilePath());

    // Create LogData with mixed keys including a timestamp
    TestLogFile testLogFile;
    std::unique_ptr<LogFile> logFile = testLogFile.CreateLogFile();
    KeyIndex testKeys;
    std::vector<LogLine> testLines;
    testLines.emplace_back(LogMap{{"regular", std::string("value")}}, testKeys, LogFileReference(*logFile, 0));
    testLines.emplace_back(LogMap{{"newKey", std::string("test")}}, testKeys, LogFileReference(*logFile, 0));
    testLines.emplace_back(
        LogMap{{"timestamp", std::string("2023-01-01T12:00:00Z")}}, testKeys, LogFileReference(*logFile, 0)
    );

    LogData logData(std::move(logFile), std::move(testLines), std::move(testKeys));

    // Update configuration with the new data
    manager.Update(logData);

    // Verify configuration structure
    REQUIRE(manager.Configuration().columns.size() == 3);

    // First column should be the timestamp
    CHECK(manager.Configuration().columns[0].header == "timestamp");
    CHECK(manager.Configuration().columns[0].type == LogConfiguration::Type::time);

    // The other columns should follow
    CHECK(manager.Configuration().columns[1].header == "regular");
    CHECK(manager.Configuration().columns[2].header == "newKey");

    // Verify timestamp has proper format settings
    CHECK(manager.Configuration().columns[0].printFormat == "%F %H:%M:%S");
    REQUIRE(manager.Configuration().columns[0].parseFormats.size() == 4);
    CHECK(manager.Configuration().columns[0].parseFormats[0] == "%FT%T%Ez");
}

// PRD §4.7.6 / parser-perf task 8.6 — `IsKeyInAnyColumn` cache invalidation tests.
//
// The cache is private; we exercise it indirectly by observing that mutating
// paths still produce the right column shape after a query has populated it.
// Specifically, every mutating path (`Load`, `Update`, `AppendKeys`) must flip
// the cache-stale flag so the next query sees the freshly mutated column set.
// If a mutating path forgets to invalidate, the loops below would either
// skip-add a column (false-positive cached "already there") or double-add a
// column (false-negative cached "absent" after the rebuild already saw it).

TEST_CASE("Cache: AppendKeys after a query sees the freshly appended key", "[LogConfigurationManager][cache_invalidation]")
{
    LogConfigurationManager manager;

    // First AppendKeys call against an empty configuration. This builds the
    // cache from an empty column set; the cache must stay coherent as we
    // append a new column inside the AppendKeys loop.
    manager.AppendKeys({"alpha"});
    REQUIRE(manager.Configuration().columns.size() == 1);
    CHECK(manager.Configuration().columns[0].header == "alpha");

    // Second AppendKeys with a mix of the existing key + a new one. The
    // existing-key check must hit the cache (so we do NOT add a duplicate
    // "alpha" column) AND the new key must be added — i.e. AppendKeys keeps
    // the in-flight cache consistent. A regression that forgets to insert the
    // freshly added key into `mKeysInColumns` mid-loop would still be caught
    // by the duplicate-key assertion.
    manager.AppendKeys({"alpha", "beta", "alpha"});
    REQUIRE(manager.Configuration().columns.size() == 2);
    CHECK(manager.Configuration().columns[0].header == "alpha");
    CHECK(manager.Configuration().columns[1].header == "beta");
}

TEST_CASE("Cache: Update after AppendKeys auto-promotes timestamps with the cache fresh", "[LogConfigurationManager][cache_invalidation]")
{
    LogConfigurationManager manager;

    // Seed via AppendKeys so the cache is built and lives across the next
    // mutator. Without invalidation, the subsequent Update would either
    // re-add "regular" (cache says "absent") or skip "timestamp" (cache says
    // "present"); the assertions below catch both.
    manager.AppendKeys({"regular"});
    REQUIRE(manager.Configuration().columns.size() == 1);

    // Build a LogData with a fresh timestamp key and a duplicate of the
    // existing key. Update must (a) skip "regular" via the cache, (b) add
    // "timestamp" as a Type::time column, and (c) shuffle it to position 0.
    TestLogFile testLogFile;
    std::unique_ptr<LogFile> logFile = testLogFile.CreateLogFile();
    KeyIndex testKeys;
    std::vector<LogLine> testLines;
    testLines.emplace_back(LogMap{{"regular", std::string("value")}}, testKeys, LogFileReference(*logFile, 0));
    testLines.emplace_back(
        LogMap{{"timestamp", std::string("2023-01-01T12:00:00Z")}}, testKeys, LogFileReference(*logFile, 0)
    );
    LogData logData(std::move(logFile), std::move(testLines), std::move(testKeys));

    manager.Update(logData);

    REQUIRE(manager.Configuration().columns.size() == 2);
    CHECK(manager.Configuration().columns[0].header == "timestamp");
    CHECK(manager.Configuration().columns[0].type == LogConfiguration::Type::time);
    CHECK(manager.Configuration().columns[1].header == "regular");
}

TEST_CASE("Cache: Load wholesale-replaces the cache against the freshly loaded columns", "[LogConfigurationManager][cache_invalidation]")
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

    // Mutate the in-memory configuration before any Load so the cache is
    // primed against keys that are not in the on-disk configuration. When
    // Load replaces the configuration wholesale, a stale cache would still
    // claim "stale_key" is present and the subsequent AppendKeys below would
    // therefore skip adding it. The test asserts the post-Load cache reflects
    // ONLY the loaded configuration.
    manager.AppendKeys({"stale_key"});
    REQUIRE(manager.Configuration().columns.size() == 1);
    CHECK(manager.Configuration().columns[0].header == "stale_key");

    manager.Load(firstConfigOnDisk.GetFilePath());
    REQUIRE(manager.Configuration().columns.size() == 1);
    CHECK(manager.Configuration().columns[0].header == "loaded_key");

    // The pre-Load `stale_key` is gone — AppendKeys MUST re-add it. A cache
    // that survived Load would still claim stale_key is present and skip it.
    manager.AppendKeys({"stale_key", "loaded_key"});
    REQUIRE(manager.Configuration().columns.size() == 2);
    CHECK(manager.Configuration().columns[0].header == "loaded_key");
    CHECK(manager.Configuration().columns[1].header == "stale_key");

    // A second Load against a different on-disk config replaces the column
    // set again; the cache must once again reflect ONLY the loaded keys.
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
    "Cache: parity with the pre-cache O(M·K) walk on the existing fixture", "[LogConfigurationManager][cache_invalidation]"
)
{
    // Re-run the existing "Update with mixed keys organizes timestamp first"
    // shape and assert the column-set parity. This is the regression boundary
    // between the old `IsKeyInAnyColumn` free function and the cached member.
    TestLogConfiguration testLogConfiguration;
    LogConfiguration logConfiguration;
    logConfiguration.columns.push_back({"regular", {"regular"}, "{}", LogConfiguration::Type::any, {}});
    testLogConfiguration.Write(logConfiguration);

    LogConfigurationManager manager;
    manager.Load(testLogConfiguration.GetFilePath());

    TestLogFile testLogFile;
    std::unique_ptr<LogFile> logFile = testLogFile.CreateLogFile();
    KeyIndex testKeys;
    std::vector<LogLine> testLines;
    testLines.emplace_back(LogMap{{"regular", std::string("value")}}, testKeys, LogFileReference(*logFile, 0));
    testLines.emplace_back(LogMap{{"newKey", std::string("test")}}, testKeys, LogFileReference(*logFile, 0));
    testLines.emplace_back(
        LogMap{{"timestamp", std::string("2023-01-01T12:00:00Z")}}, testKeys, LogFileReference(*logFile, 0)
    );
    LogData logData(std::move(logFile), std::move(testLines), std::move(testKeys));

    manager.Update(logData);

    REQUIRE(manager.Configuration().columns.size() == 3);
    CHECK(manager.Configuration().columns[0].header == "timestamp");
    CHECK(manager.Configuration().columns[1].header == "regular");
    CHECK(manager.Configuration().columns[2].header == "newKey");
}

TEST_CASE("Cache: a column whose `keys` differs from `header` is matched by key", "[LogConfigurationManager][cache_invalidation]")
{
    // The cache mirrors `column.keys`, not `column.header`. Catch the easy
    // mistake of caching headers instead of keys: build a column whose
    // header and keys diverge, then assert AppendKeys does NOT double-add a
    // column for any key already listed under another column's `keys`.
    TestLogConfiguration testLogConfiguration;
    LogConfiguration logConfiguration;
    logConfiguration.columns.push_back({"display", {"raw_key", "alias"}, "{}", LogConfiguration::Type::any, {}});
    testLogConfiguration.Write(logConfiguration);

    LogConfigurationManager manager;
    manager.Load(testLogConfiguration.GetFilePath());

    manager.AppendKeys({"display", "raw_key", "alias", "fresh"});

    // Existing column stays put; only "display" and "fresh" are new (header
    // "display" is itself not in any column's keys, so it gets a column).
    // "raw_key" and "alias" are matched against the existing column's keys
    // and are skipped.
    REQUIRE(manager.Configuration().columns.size() == 3);
    CHECK(manager.Configuration().columns[0].header == "display");
    CHECK(manager.Configuration().columns[1].header == "display"); // header reused; keys differ
    CHECK(manager.Configuration().columns[2].header == "fresh");
    CHECK(manager.Configuration().columns[1].keys == std::vector<std::string>{"display"});
    CHECK(manager.Configuration().columns[2].keys == std::vector<std::string>{"fresh"});
}
