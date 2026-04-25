#include "common.hpp"

#include <loglib/key_index.hpp>
#include <loglib/log_file.hpp>
#include <loglib/log_line.hpp>

#include <catch2/catch_all.hpp>

using namespace loglib;

TEST_CASE("Construct LogLine with valid values and file reference", "[log_line]")
{
    LogMap map{
        {"key1", std::string("value1")},
        {"key2", uint64_t(42)},
        {"key3", int64_t(-12)},
        {"key4", 3.14},
        {"key5", true},
        {"key6", std::monostate()}
    };

    TestLogFile testLogFile;
    testLogFile.Write("abcd\nefgh\n");
    std::unique_ptr<LogFile> logFile = testLogFile.CreateLogFile();
    LogFileReference fileReference(*logFile, 1);

    KeyIndex keys;
    LogLine line(map, keys, fileReference);

    REQUIRE(line.Values().size() == map.size());
    CHECK(std::get<std::string>(line.GetValue("key1")) == "value1");
    CHECK(std::get<uint64_t>(line.GetValue("key2")) == 42);
    CHECK(std::get<int64_t>(line.GetValue("key3")) == -12);
    CHECK(std::get<double>(line.GetValue("key4")) == Catch::Approx(3.14));
    CHECK(std::get<bool>(line.GetValue("key5")) == true);
    CHECK(std::holds_alternative<std::monostate>(line.GetValue("key6")));

    CHECK(line.FileReference().GetPath() == "test_file.json");
    CHECK(line.FileReference().GetLineNumber() == 1);
    CHECK(line.FileReference().GetLine() == "efgh");

    auto resultKeys = line.GetKeys();
    REQUIRE(resultKeys.size() == map.size());
    CHECK(std::find(resultKeys.begin(), resultKeys.end(), "key1") != resultKeys.end());
    CHECK(std::find(resultKeys.begin(), resultKeys.end(), "key2") != resultKeys.end());
    CHECK(std::find(resultKeys.begin(), resultKeys.end(), "key3") != resultKeys.end());
    CHECK(std::find(resultKeys.begin(), resultKeys.end(), "key4") != resultKeys.end());
    CHECK(std::find(resultKeys.begin(), resultKeys.end(), "key5") != resultKeys.end());
    CHECK(std::find(resultKeys.begin(), resultKeys.end(), "key6") != resultKeys.end());

    const LogMap resultMap = line.Values();

    REQUIRE(resultMap.size() == map.size());

    for (const auto &[key, expectedValue] : map)
    {
        REQUIRE(resultMap.count(key) == 1);

        std::visit(
            [&](const auto &expected) {
                using T = std::decay_t<decltype(expected)>;
                const auto &actual = std::get<T>(resultMap.at(key));

                if constexpr (std::is_same_v<T, double>)
                    CHECK(actual == Catch::Approx(expected));
                else
                    CHECK(actual == expected);
            },
            expectedValue
        );
    }
}

TEST_CASE("LogLine GetKeys returns empty vector for empty LogLine", "[log_line]")
{
    LogMap emptyMap;

    TestLogFile testLogFile;
    std::unique_ptr<LogFile> logFile = testLogFile.CreateLogFile();
    LogFileReference fileReference(*logFile, 0);

    KeyIndex keys;
    LogLine emptyLine(emptyMap, keys, fileReference);

    auto resultKeys = emptyLine.GetKeys();
    CHECK(resultKeys.empty());
    CHECK(emptyLine.Values().empty());
}

TEST_CASE("LogLine returns monostate for empty and non-existent key", "[log_line]")
{
    LogMap map{{"key1", std::string("value1")}, {"key2", uint64_t(42)}};

    TestLogFile testLogFile;
    std::unique_ptr<LogFile> logFile = testLogFile.CreateLogFile();
    LogFileReference fileReference(*logFile, 0);

    KeyIndex keys;
    LogLine line(map, keys, fileReference);

    CHECK(std::holds_alternative<std::monostate>(line.GetValue("")));
    CHECK(std::holds_alternative<std::monostate>(line.GetValue("non_existent_key")));

    CHECK(std::get<std::string>(line.GetValue("key1")) == "value1");
    CHECK(std::get<uint64_t>(line.GetValue("key2")) == 42);
}

TEST_CASE("Set and update values", "[log_line]")
{
    LogMap map{{"existingKey", std::string("initialValue")}};

    TestLogFile testLogFile;
    std::unique_ptr<LogFile> logFile = testLogFile.CreateLogFile();
    LogFileReference fileReference(*logFile, 0);

    KeyIndex keys;
    LogLine line(map, keys, fileReference);

    CHECK(std::get<std::string>(line.GetValue("existingKey")) == "initialValue");
    CHECK(line.Values().size() == 1);

    const std::string updatedValue = "updatedValue";
    line.SetValue("existingKey", updatedValue);

    CHECK(std::get<std::string>(line.GetValue("existingKey")) == updatedValue);
    CHECK(line.Values().size() == 1);

    auto resultKeys = line.GetKeys();
    REQUIRE(resultKeys.size() == 1);
    CHECK(resultKeys[0] == "existingKey");

    // SetValue(string) requires the key to be registered. Pre-insert it via the KeyIndex
    // because LogLine intentionally refuses to silently grow the dictionary on the slow path.
    const std::string newKey = "newKey";
    const std::string newValue = "newValue";
    keys.GetOrInsert(newKey);
    line.SetValue(newKey, newValue);

    REQUIRE(line.Values().size() == 2);
    CHECK(std::get<std::string>(line.GetValue(newKey)) == newValue);

    resultKeys = line.GetKeys();
    REQUIRE(resultKeys.size() == 2);
    CHECK(std::find(resultKeys.begin(), resultKeys.end(), newKey) != resultKeys.end());
}
