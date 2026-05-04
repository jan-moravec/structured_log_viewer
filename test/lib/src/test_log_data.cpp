#include "common.hpp"

#include <loglib/file_line_source.hpp>
#include <loglib/key_index.hpp>
#include <loglib/log_data.hpp>
#include <loglib/log_file.hpp>
#include <loglib/log_line.hpp>

#include <catch2/catch_all.hpp>

#include <memory>
#include <string>
#include <vector>

using namespace loglib;

TEST_CASE(
    "Constructor should correctly initialize LogData with a single source and provided lines and keys", "[LogData]"
)
{
    TestLogFile testLogFile;
    auto source = testLogFile.CreateFileLineSource();
    FileLineSource *sourcePtr = source.get();

    KeyIndex testKeys;
    std::vector<LogLine> testLines;
    testLines.emplace_back(LogMap{{"key1", std::string("value1")}}, testKeys, *sourcePtr, 0);
    testLines.emplace_back(LogMap{{"key2", int64_t{42}}}, testKeys, *sourcePtr, 0);

    LogData data(std::move(source), std::move(testLines), std::move(testKeys));

    REQUIRE(data.Sources().size() == 1);
    REQUIRE(data.Sources()[0]->Path() == testLogFile.GetFilePath());
    REQUIRE(data.FrontFileSource() != nullptr);
    REQUIRE(data.FrontFileSource()->File().GetPath() == testLogFile.GetFilePath());

    REQUIRE(data.Lines().size() == 2);
    CHECK(std::get<std::string>(data.Lines()[0].GetValue("key1")) == "value1");
    CHECK(std::get<int64_t>(data.Lines()[1].GetValue("key2")) == 42);

    const auto sortedKeys = data.SortedKeys();
    REQUIRE(sortedKeys.size() == 2);
    CHECK(sortedKeys[0] == "key1");
    CHECK(sortedKeys[1] == "key2");
}

TEST_CASE("Merge() should correctly combine sources, lines and keys from two LogData objects", "[LogData]")
{
    TestLogFile testLogFile1("test_file_1.json");
    TestLogFile testLogFile2("test_file_2.json");

    auto source1 = testLogFile1.CreateFileLineSource();
    FileLineSource *source1Ptr = source1.get();
    KeyIndex keys1;
    std::vector<LogLine> lines1;
    lines1.emplace_back(LogMap{{"key1", std::string("value1")}}, keys1, *source1Ptr, 0);

    LogData data1(std::move(source1), std::move(lines1), std::move(keys1));

    auto source2 = testLogFile2.CreateFileLineSource();
    FileLineSource *source2Ptr = source2.get();
    KeyIndex keys2;
    // Pre-seed an extra (duplicate) key so we cover the remap-of-already-known-key path.
    static_cast<void>(keys2.GetOrInsert("key1"));
    std::vector<LogLine> lines2;
    lines2.emplace_back(LogMap{{"key2", int64_t{42}}}, keys2, *source2Ptr, 0);

    LogData data2(std::move(source2), std::move(lines2), std::move(keys2));

    const auto path1 = data1.Sources()[0]->Path();
    const auto path2 = data2.Sources()[0]->Path();

    data1.Merge(std::move(data2));

    REQUIRE(data1.Sources().size() == 2);
    CHECK(data1.Sources()[0]->Path() == path1);
    CHECK(data1.Sources()[1]->Path() == path2);

    REQUIRE(data1.Lines().size() == 2);
    CHECK(std::get<std::string>(data1.Lines()[0].GetValue("key1")) == "value1");
    CHECK(std::get<int64_t>(data1.Lines()[1].GetValue("key2")) == 42);

    const auto sortedKeys = data1.SortedKeys();
    REQUIRE(sortedKeys.size() == 2);
    CHECK(sortedKeys[0] == "key1");
    CHECK(sortedKeys[1] == "key2");
}
