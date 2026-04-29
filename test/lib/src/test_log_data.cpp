#include "common.hpp"

#include <loglib/key_index.hpp>
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
    TestLogFile testLogFile;
    std::unique_ptr<LogFile> logFile = testLogFile.CreateLogFile();

    KeyIndex testKeys;
    std::vector<LogLine> testLines;
    testLines.emplace_back(LogMap{{"key1", std::string("value1")}}, testKeys, LogFileReference(*logFile, 0));
    testLines.emplace_back(LogMap{{"key2", int64_t{42}}}, testKeys, LogFileReference(*logFile, 0));

    LogData data(std::move(logFile), std::move(testLines), std::move(testKeys));

    REQUIRE(data.Files().size() == 1);
    REQUIRE(data.Files()[0]->GetPath() == testLogFile.GetFilePath());

    REQUIRE(data.Lines().size() == 2);
    CHECK(std::get<std::string>(data.Lines()[0].GetValue("key1")) == "value1");
    CHECK(std::get<int64_t>(data.Lines()[1].GetValue("key2")) == 42);

    const auto sortedKeys = data.SortedKeys();
    REQUIRE(sortedKeys.size() == 2);
    CHECK(sortedKeys[0] == "key1");
    CHECK(sortedKeys[1] == "key2");
}

TEST_CASE("Merge() should correctly combine files, lines and keys from two LogData objects", "[LogData]")
{
    TestLogFile testLogFile1("test_file_1.json");
    TestLogFile testLogFile2("test_file_2.json");

    std::unique_ptr<LogFile> logFile1 = testLogFile1.CreateLogFile();
    KeyIndex keys1;
    std::vector<LogLine> lines1;
    lines1.emplace_back(LogMap{{"key1", std::string("value1")}}, keys1, LogFileReference(*logFile1, 0));

    LogData data1(std::move(logFile1), std::move(lines1), std::move(keys1));

    std::unique_ptr<LogFile> logFile2 = testLogFile2.CreateLogFile();
    KeyIndex keys2;
    // Pre-seed an extra (duplicate) key so we cover the remap-of-already-known-key path.
    static_cast<void>(keys2.GetOrInsert("key1"));
    std::vector<LogLine> lines2;
    lines2.emplace_back(LogMap{{"key2", int64_t{42}}}, keys2, LogFileReference(*logFile2, 0));

    LogData data2(std::move(logFile2), std::move(lines2), std::move(keys2));

    const auto path1 = data1.Files()[0]->GetPath();
    const auto path2 = data2.Files()[0]->GetPath();

    data1.Merge(std::move(data2));

    REQUIRE(data1.Files().size() == 2);
    CHECK(data1.Files()[0]->GetPath() == path1);
    CHECK(data1.Files()[1]->GetPath() == path2);

    REQUIRE(data1.Lines().size() == 2);
    CHECK(std::get<std::string>(data1.Lines()[0].GetValue("key1")) == "value1");
    CHECK(std::get<int64_t>(data1.Lines()[1].GetValue("key2")) == 42);

    const auto sortedKeys = data1.SortedKeys();
    REQUIRE(sortedKeys.size() == 2);
    CHECK(sortedKeys[0] == "key1");
    CHECK(sortedKeys[1] == "key2");
}
