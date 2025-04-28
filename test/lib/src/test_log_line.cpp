#include "common.hpp"

#include <loglib/log_file.hpp>
#include <loglib/log_line.hpp>

#include <catch2/catch_all.hpp>
#include <nlohmann/json.hpp>

using namespace loglib;

TEST_CASE("Construct LogLine with valid values and file reference", "[log_line]")
{
    // Create test values
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

    // Construct the LogLine
    LogLine line(map, fileReference);

    // Verify values are stored correctly
    REQUIRE(line.Values().size() == map.size());
    CHECK(std::get<std::string>(line.GetValue("key1")) == "value1");
    CHECK(std::get<uint64_t>(line.GetValue("key2")) == 42);
    CHECK(std::get<int64_t>(line.GetValue("key3")) == -12);
    CHECK(std::get<double>(line.GetValue("key4")) == Catch::Approx(3.14));
    CHECK(std::get<bool>(line.GetValue("key5")) == true);
    CHECK(std::holds_alternative<std::monostate>(line.GetValue("key6")));

    // Verify file reference is stored correctly
    CHECK(line.FileReference().GetPath() == "test_file.json");
    CHECK(line.FileReference().GetLineNumber() == 1);
    CHECK(line.FileReference().GetLine() == "efgh");

    // Verify keys can be retrieved
    auto keys = line.GetKeys();
    REQUIRE(keys.size() == map.size());
    CHECK(std::find(keys.begin(), keys.end(), "key1") != keys.end());
    CHECK(std::find(keys.begin(), keys.end(), "key2") != keys.end());
    CHECK(std::find(keys.begin(), keys.end(), "key3") != keys.end());
    CHECK(std::find(keys.begin(), keys.end(), "key4") != keys.end());
    CHECK(std::find(keys.begin(), keys.end(), "key5") != keys.end());
    CHECK(std::find(keys.begin(), keys.end(), "key6") != keys.end());

    // Get the values map
    const LogMap &resultMap = line.Values();

    // Verify the returned map matches the original
    REQUIRE(resultMap.size() == map.size());

    // Check each key-value pair
    for (const auto &[key, expectedValue] : map)
    {
        REQUIRE(resultMap.count(key) == 1);

        // Compare values based on type
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
    // Create an empty LogMap
    LogMap emptyMap;

    // Create a minimal LogFileReference
    TestLogFile testLogFile;
    std::unique_ptr<LogFile> logFile = testLogFile.CreateLogFile();
    LogFileReference fileReference(*logFile, 0);

    // Construct a LogLine with empty values
    LogLine emptyLine(emptyMap, fileReference);

    // Verify GetKeys returns an empty vector
    auto keys = emptyLine.GetKeys();
    CHECK(keys.empty());

    // Double-check that the values are indeed empty
    CHECK(emptyLine.Values().empty());
}

TEST_CASE("LogLine returns monostate for empty and non-existent key", "[log_line]")
{
    // Create test values with some data
    LogMap map{{"key1", std::string("value1")}, {"key2", uint64_t(42)}};

    // Create minimum required file reference
    TestLogFile testLogFile;
    std::unique_ptr<LogFile> logFile = testLogFile.CreateLogFile();
    LogFileReference fileReference(*logFile, 0);

    // Construct the LogLine
    LogLine line(map, fileReference);

    // Verify empty key returns a monostate
    CHECK(std::holds_alternative<std::monostate>(line.GetValue("")));

    // Test getting a value for a non-existent key
    CHECK(std::holds_alternative<std::monostate>(line.GetValue("non_existent_key")));

    // Verify that the known keys still return their expected values
    CHECK(std::get<std::string>(line.GetValue("key1")) == "value1");
    CHECK(std::get<uint64_t>(line.GetValue("key2")) == 42);
}

TEST_CASE("Set and update values", "[log_line]")
{
    LogMap map{{"existingKey", std::string("initialValue")}};

    // Create a test file for the LogFileReference
    TestLogFile testLogFile;
    std::unique_ptr<LogFile> logFile = testLogFile.CreateLogFile();
    LogFileReference fileReference(*logFile, 0);

    // Create a LogLine instance
    LogLine line(map, fileReference);

    // Verify the initial value is set correctly
    CHECK(std::get<std::string>(line.GetValue("existingKey")) == "initialValue");
    CHECK(line.Values().size() == 1);

    // Update the value for the existing key
    const std::string updatedValue = "updatedValue";
    line.SetValue("existingKey", updatedValue);

    // Verify the value was updated correctly
    CHECK(std::get<std::string>(line.GetValue("existingKey")) == updatedValue);

    // Verify that we still have only one key in the map
    CHECK(line.Values().size() == 1);

    // Verify the key still appears in GetKeys
    auto keys = line.GetKeys();
    REQUIRE(keys.size() == 1);
    CHECK(keys[0] == "existingKey");

    // Set a value for a non-existent key
    const std::string newKey = "newKey";
    const std::string newValue = "newValue";
    line.SetValue(newKey, newValue);

    // Verify the value was added correctly
    REQUIRE(line.Values().size() == 2);
    CHECK(std::get<std::string>(line.GetValue(newKey)) == newValue);

    // Verify the key appears in GetKeys
    keys = line.GetKeys();
    REQUIRE(keys.size() == 2);
    CHECK(std::find(keys.begin(), keys.end(), newKey) != keys.end());
}
