#include "common.hpp"

#include <loglib/core/file_line_source.hpp>
#include <loglib/core/key_index.hpp>
#include <loglib/core/log_data.hpp>
#include <loglib/core/log_file.hpp>
#include <loglib/core/log_line.hpp>

#include <catch2/catch_all.hpp>

#include <memory>
#include <string>
#include <vector>

using namespace loglib;

TEST_CASE(
    "Constructor should correctly initialize LogData with a single source and provided lines and keys", "[LogData]"
)
{
    const TestLogFile testLogFile;
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
    REQUIRE(data.FrontFileSource()->File().Path() == testLogFile.GetFilePath());

    REQUIRE(data.Lines().size() == 2);
    CHECK(std::get<std::string>(data.Lines()[0].GetValue("key1")) == "value1");
    CHECK(std::get<int64_t>(data.Lines()[1].GetValue("key2")) == 42);

    const auto sortedKeys = data.SortedKeys();
    REQUIRE(sortedKeys.size() == 2);
    CHECK(sortedKeys[0] == "key1");
    CHECK(sortedKeys[1] == "key2");
}
