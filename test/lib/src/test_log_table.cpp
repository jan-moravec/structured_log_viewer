#include "common.hpp"

#include <loglib/log_configuration.hpp>
#include <loglib/log_data.hpp>
#include <loglib/log_table.hpp>

#include <catch2/catch_all.hpp>

using namespace loglib;

TEST_CASE("Initialize a LogTable with given LogData and LogConfigurationManager", "[log_table]")
{
    // Setup test data
    TestLogFile testFile;
    testFile.Write("line1\nline2");
    std::unique_ptr<LogFile> logFile = testFile.CreateLogFile();

    // Create test log lines
    std::vector<LogLine> testLines;
    testLines.emplace_back(LogMap{{"key1", "value1"}}, LogFileReference(*logFile, 0));
    testLines.emplace_back(LogMap{{"key2", "value2"}}, LogFileReference(*logFile, 1));

    // Create test keys
    std::vector<std::string> testKeys = {"key1", "key2"};

    // Create LogData instance
    LogData logData(std::move(logFile), std::move(testLines), std::move(testKeys));

    // Create test configuration
    LogConfiguration logConfiguration;
    logConfiguration.columns.push_back({"Header1", {"key1"}, "{}", LogConfiguration::Type::any, {}});
    logConfiguration.columns.push_back({"Header2", {"key2"}, "{}", LogConfiguration::Type::any, {}});
    TestLogConfiguration testLogConfiguration;
    testLogConfiguration.Write(logConfiguration);
    LogConfigurationManager manager;
    manager.Load(testLogConfiguration.GetFilePath());

    // Initialize LogTable with the data and configuration
    LogTable logTable(std::move(logData), std::move(manager));

    // Verify LogTable correctly initialized with the provided data and configuration
    REQUIRE(logTable.RowCount() == 2);
    REQUIRE(logTable.ColumnCount() == 2);
    CHECK(logTable.GetHeader(0) == "Header1");
    CHECK(logTable.GetHeader(1) == "Header2");
    CHECK(std::get<std::string>(logTable.GetValue(0, 0)) == "value1");
    CHECK(std::get<std::string>(logTable.GetValue(1, 1)) == "value2");
    REQUIRE(logTable.Data().Files().size() == 1);
    REQUIRE(logTable.Data().Lines().size() == 2);
    CHECK(logTable.Data().Lines()[0].FileReference().GetLineNumber() == 0);
    CHECK(logTable.Data().Lines()[0].FileReference().GetLine() == "line1");
    CHECK(logTable.Data().Lines()[0].FileReference().GetPath() == testFile.GetFilePath());
    CHECK(logTable.Data().Lines()[1].FileReference().GetLineNumber() == 1);
    CHECK(logTable.Data().Lines()[1].FileReference().GetLine() == "line2");
    CHECK(logTable.Data().Lines()[1].FileReference().GetPath() == testFile.GetFilePath());
}

TEST_CASE("Update LogTable with new LogData", "[log_table]")
{
    TestLogFile testFile("log_file.json");
    TestLogFile newTestFile("new_log_file.json");

    // Setup initial test data
    testFile.Write("file1\nfile2");
    std::unique_ptr<LogFile> logFile = testFile.CreateLogFile();

    // Create initial log lines
    std::vector<LogLine> initialLines;
    initialLines.emplace_back(LogMap{{"key1", "value1"}}, LogFileReference(*logFile, 0));

    // Create initial test keys
    std::vector<std::string> initialKeys = {"key1"};

    // Create initial LogData instance
    LogData initialData(std::move(logFile), std::move(initialLines), std::move(initialKeys));

    // Create initial configuration
    LogConfiguration logConfiguration;
    logConfiguration.columns.push_back({"Header1", {"key1"}, "{}", LogConfiguration::Type::any, {}});
    TestLogConfiguration testLogConfiguration;
    testLogConfiguration.Write(logConfiguration);
    LogConfigurationManager manager;
    manager.Load(testLogConfiguration.GetFilePath());

    // Initialize LogTable with the initial data and configuration
    LogTable logTable(std::move(initialData), std::move(manager));

    // Verify initial state
    REQUIRE(logTable.RowCount() == 1);
    REQUIRE(logTable.ColumnCount() == 1);
    CHECK(logTable.GetHeader(0) == "Header1");
    CHECK(std::get<std::string>(logTable.GetValue(0, 0)) == "value1");

    // Create new test data to update with
    newTestFile.Write("newfile1\nnewfile2");
    std::unique_ptr<LogFile> newLogFile = newTestFile.CreateLogFile();

    // Create new log lines with new keys
    std::vector<LogLine> newLines;
    newLines.emplace_back(LogMap{{"key2", "value2"}}, LogFileReference(*newLogFile, 0));

    // Create new test keys
    std::vector<std::string> newKeys = {"key2"};

    // Create new LogData instance
    LogData newData(std::move(newLogFile), std::move(newLines), std::move(newKeys));

    // Update the LogTable with the new data
    logTable.Update(std::move(newData));

    // Verify the LogTable was updated correctly
    REQUIRE(logTable.RowCount() == 2);
    REQUIRE(logTable.ColumnCount() == 2); // Configuration should be updated with the new key
    CHECK(logTable.GetHeader(0) == "Header1");
    CHECK(logTable.GetHeader(1) == "key2"); // New column should be added with default header matching key name
    CHECK(std::get<std::string>(logTable.GetValue(0, 0)) == "value1");
    CHECK(std::get<std::string>(logTable.GetValue(1, 1)) == "value2");
    REQUIRE(logTable.Data().Files().size() == 2);
    REQUIRE(logTable.Data().Lines().size() == 2);
    CHECK(logTable.Data().Lines()[0].FileReference().GetLineNumber() == 0);
    CHECK(logTable.Data().Lines()[0].FileReference().GetLine() == "file1");
    CHECK(logTable.Data().Lines()[0].FileReference().GetPath() == testFile.GetFilePath());
    CHECK(logTable.Data().Lines()[1].FileReference().GetLineNumber() == 0);
    CHECK(logTable.Data().Lines()[1].FileReference().GetLine() == "newfile1");
    CHECK(logTable.Data().Lines()[1].FileReference().GetPath() == newTestFile.GetFilePath());
}
