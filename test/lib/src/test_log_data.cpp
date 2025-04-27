#include "common.hpp"

#include <loglib/log_data.hpp>
#include <loglib/log_file.hpp>
#include <loglib/log_line.hpp>

#include <catch2/catch_all.hpp>

#include <memory>
#include <string>
#include <vector>

using namespace loglib;

TEST_CASE("Constructor should correctly initialize LogData with a single file and provided lines and keys", "[LogData]")
{
    // Setup test data
    TestLogFile testLogFile;
    std::unique_ptr<LogFile> logFile = testLogFile.CreateLogFile();
    std::vector<LogLine> testLines;
    testLines.emplace_back(LogMap{{"key1", "value1"}}, LogFileReference(*logFile, 0));
    testLines.emplace_back(LogMap{{"key2", 42}}, LogFileReference(*logFile, 0));
    std::vector<std::string> testKeys = {"key1", "key2"};

    // Create LogData instance
    LogData data(std::move(logFile), std::move(testLines), std::move(testKeys));

    // Verify the instance is correctly initialized
    REQUIRE(data.Files().size() == 1);
    REQUIRE(data.Files()[0]->GetPath() == testLogFile.GetFilePath());

    REQUIRE(data.Lines().size() == 2);
    CHECK(std::get<std::string>(data.Lines()[0].GetValue("key1")) == "value1");
    CHECK(std::get<int64_t>(data.Lines()[1].GetValue("key2")) == 42);

    REQUIRE(data.Keys().size() == 2);
    CHECK(data.Keys()[0] == "key1");
    CHECK(data.Keys()[1] == "key2");
}

TEST_CASE("Merge() should correctly combine files, lines and keys from two LogData objects", "[LogData]")
{
    TestLogFile testLogFile1("test_file_1.json");
    TestLogFile testLogFile2("test_file_2.json");

    // Setup first LogData object
    std::unique_ptr<LogFile> logFile1 = testLogFile1.CreateLogFile();
    std::vector<LogLine> lines1;
    lines1.emplace_back(LogMap{{"key1", "value1"}}, LogFileReference(*logFile1, 0));
    std::vector<std::string> keys1 = {"key1"};

    // Create first LogData instance
    LogData data1(std::move(logFile1), std::move(lines1), std::move(keys1));

    // Setup second LogData object
    std::unique_ptr<LogFile> logFile2 = testLogFile2.CreateLogFile();
    std::vector<LogLine> lines2;
    lines2.emplace_back(LogMap{{"key2", 42}}, LogFileReference(*logFile2, 0));
    std::vector<std::string> keys2 = {"key2", "key1"}; // Intentionally include a duplicate key

    // Create second LogData instance
    LogData data2(std::move(logFile2), std::move(lines2), std::move(keys2));

    // Get the files' paths for later verification
    const auto path1 = data1.Files()[0]->GetPath();
    const auto path2 = data2.Files()[0]->GetPath();

    // Merge data2 into data1
    data1.Merge(std::move(data2));

    // Verify merged files
    REQUIRE(data1.Files().size() == 2);
    CHECK(data1.Files()[0]->GetPath() == path1);
    CHECK(data1.Files()[1]->GetPath() == path2);

    // Verify merged lines
    REQUIRE(data1.Lines().size() == 2);
    CHECK(std::get<std::string>(data1.Lines()[0].GetValue("key1")) == "value1");
    CHECK(std::get<int64_t>(data1.Lines()[1].GetValue("key2")) == 42);

    // Verify merged keys (should remove duplicates)
    REQUIRE(data1.Keys().size() == 2);
    // Since we're using a set internally, the order is maintained from the first LogData
    // and new keys are appended
    CHECK(data1.Keys()[0] == "key1");
    CHECK(data1.Keys()[1] == "key2");
}
