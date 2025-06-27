#include "common.hpp"

#include <loglib/log_configuration.hpp>
#include <loglib/log_data.hpp>

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
    std::vector<LogLine> testLines;
    testLines.emplace_back(LogMap{{"regular", "value"}}, LogFileReference(*logFile, 0));
    testLines.emplace_back(LogMap{{"newKey", "test"}}, LogFileReference(*logFile, 0));
    testLines.emplace_back(LogMap{{"timestamp", "2023-01-01T12:00:00Z"}}, LogFileReference(*logFile, 0));
    std::vector<std::string> testKeys = {"regular", "newKey", "timestamp"};

    // Create LogData instance
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
