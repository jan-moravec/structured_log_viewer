#include "common.hpp"

#include <loglib/json_parser.hpp>
#include <loglib/log_line.hpp>

#include <catch2/catch_all.hpp>
#include <glaze/glaze.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <thread>

namespace
{
constexpr uint64_t LARGE_UINT = 10000000000000000000ULL;
} // namespace

TEST_CASE("Validate non-existent file", "[json_parser]")
{
    loglib::JsonParser parser;
    CHECK_FALSE(parser.IsValid("non_existent_file.json"));
}

TEST_CASE("Validate empty file", "[json_parser]")
{
    loglib::JsonParser parser;
    TestJsonLogFile testFile;
    CHECK_FALSE(parser.IsValid(testFile.GetFilePath()));
}

TEST_CASE("Validate file with empty lines", "[json_parser]")
{
    loglib::JsonParser parser;
    TestJsonLogFile testFile(TestJsonLogFile::Line("\n\n\n"));
    CHECK_FALSE(parser.IsValid(testFile.GetFilePath()));
}

TEST_CASE("Validate file with leading blank line", "[json_parser]")
{
    // Leading blank lines are tolerated: the first non-empty line is what determines validity.
    loglib::JsonParser parser;
    TestJsonLogFile testFile(std::vector<TestJsonLogFile::Line>{"\n", R"({"key": "value"})"});
    CHECK(parser.IsValid(testFile.GetFilePath()));
}

TEST_CASE("Validate file with invalid line", "[json_parser]")
{
    loglib::JsonParser parser;
    TestJsonLogFile testFile(TestJsonLogFile::Line("invalid json"));
    CHECK_FALSE(parser.IsValid(testFile.GetFilePath()));
}

TEST_CASE("Validate file with empty JSON object", "[json_parser]")
{
    loglib::JsonParser parser;
    TestJsonLogFile testFile(TestJsonLogFile::Line(R"({})"));
    CHECK(parser.IsValid(testFile.GetFilePath()));
}

TEST_CASE("Validate file with JSON line", "[json_parser]")
{
    loglib::JsonParser parser;
    TestJsonLogFile testFile(TestJsonLogFile::Line(R"({"key": "value"})"));
    CHECK(parser.IsValid(testFile.GetFilePath()));
}

TEST_CASE("Parse non-existent file", "[json_parser]")
{
    loglib::JsonParser parser;
    CHECK_THROWS_AS(parser.Parse("non_existent_file.json"), std::runtime_error);
}

TEST_CASE("Parse empty file", "[json_parser]")
{
    loglib::JsonParser parser;
    TestJsonLogFile testFile;
    CHECK_THROWS_AS(parser.Parse(testFile.GetFilePath()), std::runtime_error);
}

TEST_CASE("Parse file with empty lines", "[json_parser]")
{
    // Blank lines are not errors, just content-free input. Parse should succeed with no data
    // and no errors so callers can distinguish this from parse failures.
    loglib::JsonParser parser;
    TestJsonLogFile testFile(TestJsonLogFile::Line("\n\n\n"));
    auto result = parser.Parse(testFile.GetFilePath());
    CHECK(result.data.Lines().empty());
    CHECK(result.errors.empty());
}

TEST_CASE("Parse file with invalid line", "[json_parser]")
{
    // A file containing only invalid lines produces an empty LogData but reports each failure
    // through `errors` so the caller can surface them.
    loglib::JsonParser parser;
    TestJsonLogFile testFile(TestJsonLogFile::Line("invalid json"));
    auto result = parser.Parse(testFile.GetFilePath());
    CHECK(result.data.Lines().empty());
    CHECK(result.errors.size() == 1);
}

TEST_CASE("Parse file with invalid and valid line", "[json_parser]")
{
    loglib::JsonParser parser;
    TestJsonLogFile testFile(std::vector<TestJsonLogFile::Line>({"invalid json", R"({"key": "value"})"}));
    auto result = parser.Parse(testFile.GetFilePath());
    CHECK(result.errors.size() == 1);
    CHECK(result.data.Lines().size() == 1);
}

TEST_CASE("Parse file with multiple invalid lines", "[json_parser]")
{
    loglib::JsonParser parser;
    TestJsonLogFile testFile(std::vector<TestJsonLogFile::Line>{"invalid json 1", "invalid json 2"});
    auto result = parser.Parse(testFile.GetFilePath());
    CHECK(result.data.Lines().empty());
    CHECK(result.errors.size() == 2);
}

TEST_CASE("Parse file with empty JSON object", "[json_parser]")
{
    loglib::JsonParser parser;
    TestJsonLogFile testFile(TestJsonLogFile::Line(R"({})"));

    auto result = parser.Parse(testFile.GetFilePath());
    CHECK(result.errors.empty());
    REQUIRE(result.data.Lines().size() == testFile.JsonLines().size());
    CHECK(result.data.Lines()[0].IndexedValues().empty());
    CHECK(result.data.Keys().Size() == 0);
    REQUIRE(result.data.Files().size() == 1);
    CHECK(result.data.Files()[0]->GetPath() == testFile.GetFilePath());
    CHECK(result.data.Files()[0]->GetLine(0) == "{}");
}

TEST_CASE("Parse file with multiple empty JSON objects", "[json_parser]")
{
    loglib::JsonParser parser;
    TestJsonLogFile testFile(std::vector<TestJsonLogFile::Line>{R"({})", R"({})"});

    auto result = parser.Parse(testFile.GetFilePath());
    CHECK(result.errors.empty());
    REQUIRE(result.data.Lines().size() == testFile.JsonLines().size());
    CHECK(result.data.Keys().Size() == 0);
    REQUIRE(result.data.Files().size() == 1);
    CHECK(result.data.Files()[0]->GetPath() == testFile.GetFilePath());
    for (size_t i = 0; i < result.data.Lines().size(); i++)
    {
        CHECK(result.data.Lines()[i].IndexedValues().empty());
        CHECK(result.data.Files()[0]->GetLine(i) == "{}");
    }
}

TEST_CASE("Parse file with single JSON object containing single JSON element", "[json_parser]")
{
    loglib::JsonParser parser;

    SECTION("Null")
    {
        TestJsonLogFile testFile(TestJsonLogFile::Line(R"({"key": null})"));
        auto result = parser.Parse(testFile.GetFilePath());
        CHECK(result.errors.empty());
        REQUIRE(result.data.Lines().size() == testFile.JsonLines().size());
        CHECK(std::holds_alternative<std::monostate>(result.data.Lines()[0].GetValue("key")));
        REQUIRE(result.data.SortedKeys().size() == 1);
        CHECK(result.data.SortedKeys()[0] == "key");
        REQUIRE(result.data.Files().size() == 1);
        CHECK(result.data.Files()[0]->GetPath() == testFile.GetFilePath());
        CHECK(result.data.Files()[0]->GetLine(0) == R"({"key":null})");
    }

    SECTION("String")
    {
        // Unescaped string values are emitted as std::string_view into the mmap (PRD req.
        // 4.1.6/4.1.15a); use AsStringView so the test is agnostic to which alternative the
        // parser picks per the fast/slow path heuristic.
        TestJsonLogFile testFile(TestJsonLogFile::Line(R"({"key": "value"})"));
        auto result = parser.Parse(testFile.GetFilePath());
        CHECK(result.errors.empty());
        REQUIRE(result.data.Lines().size() == testFile.JsonLines().size());
        CHECK(loglib::AsStringView(result.data.Lines()[0].GetValue("key")) == std::string_view{"value"});
        REQUIRE(result.data.SortedKeys().size() == 1);
        CHECK(result.data.SortedKeys()[0] == "key");
        REQUIRE(result.data.Files().size() == 1);
        CHECK(result.data.Files()[0]->GetPath() == testFile.GetFilePath());
        CHECK(result.data.Files()[0]->GetLine(0) == R"({"key":"value"})");
    }

    SECTION("Unsigned integer")
    {
        TestJsonLogFile testFile(TestJsonLogFile::Line(R"({"key": 10000000000000000000})"));
        auto result = parser.Parse(testFile.GetFilePath());
        CHECK(result.errors.empty());
        REQUIRE(result.data.Lines().size() == testFile.JsonLines().size());
        CHECK(std::get<uint64_t>(result.data.Lines()[0].GetValue("key")) == LARGE_UINT);
        REQUIRE(result.data.SortedKeys().size() == 1);
        CHECK(result.data.SortedKeys()[0] == "key");
        REQUIRE(result.data.Files().size() == 1);
        CHECK(result.data.Files()[0]->GetPath() == testFile.GetFilePath());
        CHECK(result.data.Files()[0]->GetLine(0) == R"({"key":10000000000000000000})");
    }

    SECTION("Integer")
    {
        TestJsonLogFile testFile(TestJsonLogFile::Line(R"({"key": -12})"));
        auto result = parser.Parse(testFile.GetFilePath());
        CHECK(result.errors.empty());
        REQUIRE(result.data.Lines().size() == testFile.JsonLines().size());
        CHECK(std::get<int64_t>(result.data.Lines()[0].GetValue("key")) == -12);
        REQUIRE(result.data.SortedKeys().size() == 1);
        CHECK(result.data.SortedKeys()[0] == "key");
        REQUIRE(result.data.Files().size() == 1);
        CHECK(result.data.Files()[0]->GetPath() == testFile.GetFilePath());
        CHECK(result.data.Files()[0]->GetLine(0) == R"({"key":-12})");
    }

    SECTION("Double")
    {
        TestJsonLogFile testFile(TestJsonLogFile::Line(R"({"key": 3.14})"));
        auto result = parser.Parse(testFile.GetFilePath());
        CHECK(result.errors.empty());
        REQUIRE(result.data.Lines().size() == testFile.JsonLines().size());
        CHECK(std::get<double>(result.data.Lines()[0].GetValue("key")) == Catch::Approx(3.14));
        REQUIRE(result.data.SortedKeys().size() == 1);
        CHECK(result.data.SortedKeys()[0] == "key");
        REQUIRE(result.data.Files().size() == 1);
        CHECK(result.data.Files()[0]->GetPath() == testFile.GetFilePath());
        CHECK(result.data.Files()[0]->GetLine(0) == R"({"key":3.14})");
    }

    SECTION("Boolean")
    {
        TestJsonLogFile testFile(TestJsonLogFile::Line(R"({"key": true})"));
        auto result = parser.Parse(testFile.GetFilePath());
        REQUIRE(result.errors.empty());
        CHECK(result.data.Lines().size() == testFile.JsonLines().size());
        CHECK(std::get<bool>(result.data.Lines()[0].GetValue("key")) == true);
        REQUIRE(result.data.SortedKeys().size() == 1);
        CHECK(result.data.SortedKeys()[0] == "key");
        REQUIRE(result.data.Files().size() == 1);
        CHECK(result.data.Files()[0]->GetPath() == testFile.GetFilePath());
        CHECK(result.data.Files()[0]->GetLine(0) == R"({"key":true})");
    }
}

TEST_CASE("Parse file with single JSON object containing all possible JSON elements", "[json_parser]")
{
    loglib::JsonParser parser;
    TestJsonLogFile testFile(glz::generic_sorted_u64{
        {"null", nullptr},
        {"string", "value"},
        {"uinteger", LARGE_UINT},
        {"integer", -12},
        {"double", 3.14},
        {"boolean", true}
    });

    auto result = parser.Parse(testFile.GetFilePath());
    CHECK(result.errors.empty());
    REQUIRE(result.data.Lines().size() == testFile.JsonLines().size());

    const auto values = result.data.Lines()[0].Values();
    CHECK(std::holds_alternative<std::monostate>(values.at("null")));
    CHECK(loglib::AsStringView(values.at("string")) == std::string_view{"value"});
    CHECK(std::get<uint64_t>(values.at("uinteger")) == LARGE_UINT);
    CHECK(std::get<int64_t>(values.at("integer")) == -12);
    CHECK(std::get<double>(values.at("double")) == Catch::Approx(3.14));
    CHECK(std::get<bool>(values.at("boolean")) == true);
    const auto sortedKeys1 = result.data.SortedKeys();
    REQUIRE(sortedKeys1.size() == 6);
    CHECK(sortedKeys1[0] == "boolean");
    CHECK(sortedKeys1[1] == "double");
    CHECK(sortedKeys1[2] == "integer");
    CHECK(sortedKeys1[3] == "null");
    CHECK(sortedKeys1[4] == "string");
    CHECK(sortedKeys1[5] == "uinteger");
    REQUIRE(result.data.Files().size() == 1);
    CHECK(result.data.Files()[0]->GetPath() == testFile.GetFilePath());
    CHECK(
        result.data.Files()[0]->GetLine(0) ==
        R"({"boolean":true,"double":3.14,"integer":-12,"null":null,"string":"value","uinteger":10000000000000000000})"
    );
}

TEST_CASE("Parse different key types on different lines", "[json_parser]")
{
    // This test should validate caching
    loglib::JsonParser parser;
    TestJsonLogFile testFile(
        {glz::generic_sorted_u64{
             {"1", nullptr}, {"2", "value"}, {"3", LARGE_UINT}, {"4", -12}, {"5", 3.14}, {"6", true}
         },
         glz::generic_sorted_u64{
             {"6", nullptr}, {"1", "value"}, {"2", LARGE_UINT}, {"3", -12}, {"4", 3.14}, {"5", true}
         },
         glz::generic_sorted_u64{
             {"5", nullptr}, {"6", "value"}, {"1", LARGE_UINT}, {"2", -12}, {"3", 3.14}, {"4", true}
         }}
    );

    auto result = parser.Parse(testFile.GetFilePath());
    CHECK(result.errors.empty());
    REQUIRE(result.data.Lines().size() == testFile.JsonLines().size());

    const auto values = result.data.Lines()[0].Values();
    CHECK(std::holds_alternative<std::monostate>(values.at("1")));
    CHECK(loglib::AsStringView(values.at("2")) == std::string_view{"value"});
    CHECK(std::get<uint64_t>(values.at("3")) == LARGE_UINT);
    CHECK(std::get<int64_t>(values.at("4")) == -12);
    CHECK(std::get<double>(values.at("5")) == Catch::Approx(3.14));
    CHECK(std::get<bool>(values.at("6")) == true);

    const auto values1 = result.data.Lines()[1].Values();
    CHECK(std::holds_alternative<std::monostate>(values1.at("6")));
    CHECK(loglib::AsStringView(values1.at("1")) == std::string_view{"value"});
    CHECK(std::get<uint64_t>(values1.at("2")) == LARGE_UINT);
    CHECK(std::get<int64_t>(values1.at("3")) == -12);
    CHECK(std::get<double>(values1.at("4")) == Catch::Approx(3.14));
    CHECK(std::get<bool>(values1.at("5")) == true);

    const auto values2 = result.data.Lines()[2].Values();
    CHECK(std::holds_alternative<std::monostate>(values2.at("5")));
    CHECK(loglib::AsStringView(values2.at("6")) == std::string_view{"value"});
    CHECK(std::get<uint64_t>(values2.at("1")) == LARGE_UINT);
    CHECK(std::get<int64_t>(values2.at("2")) == -12);
    CHECK(std::get<double>(values2.at("3")) == Catch::Approx(3.14));
    CHECK(std::get<bool>(values2.at("4")) == true);

    const auto sortedKeys2 = result.data.SortedKeys();
    REQUIRE(sortedKeys2.size() == 6);
    CHECK(sortedKeys2[0] == "1");
    CHECK(sortedKeys2[1] == "2");
    CHECK(sortedKeys2[2] == "3");
    CHECK(sortedKeys2[3] == "4");
    CHECK(sortedKeys2[4] == "5");
    CHECK(sortedKeys2[5] == "6");

    REQUIRE(result.data.Files().size() == 1);
    CHECK(result.data.Files()[0]->GetPath() == testFile.GetFilePath());
}

TEST_CASE("Parse file with multiple JSON objects", "[json_parser]")
{
    loglib::JsonParser parser;
    TestJsonLogFile testFile({glz::generic_sorted_u64{{"key1", "value1"}}, glz::generic_sorted_u64{{"key2", "value2"}}}
    );

    auto result = parser.Parse(testFile.GetFilePath());
    CHECK(result.errors.empty());
    REQUIRE(result.data.Lines().size() == testFile.JsonLines().size());
    CHECK(loglib::AsStringView(result.data.Lines()[0].GetValue("key1")) == std::string_view{"value1"});
    CHECK(loglib::AsStringView(result.data.Lines()[1].GetValue("key2")) == std::string_view{"value2"});
    const auto sortedKeys = result.data.SortedKeys();
    REQUIRE(sortedKeys.size() == 2);
    CHECK(sortedKeys[0] == "key1");
    CHECK(sortedKeys[1] == "key2");
    REQUIRE(result.data.Files().size() == 1);
    CHECK(result.data.Files()[0]->GetPath() == testFile.GetFilePath());
    CHECK(result.data.Files()[0]->GetLine(0) == R"({"key1":"value1"})");
    CHECK(result.data.Files()[0]->GetLine(1) == R"({"key2":"value2"})");
}

TEST_CASE("Parse file with multiple JSON objects and one invalid line", "[json_parser]")
{
    // Parse will end with first invalid line
    loglib::JsonParser parser;
    TestJsonLogFile testFile(
        {glz::generic_sorted_u64{{"key1", "value1"}}, "invalid json", glz::generic_sorted_u64{{"key2", "value2"}}}
    );

    auto result = parser.Parse(testFile.GetFilePath());
    CHECK(result.errors.size() == 1);
    REQUIRE(result.data.Lines().size() == 2);
    CHECK(loglib::AsStringView(result.data.Lines()[0].GetValue("key1")) == std::string_view{"value1"});
    CHECK(loglib::AsStringView(result.data.Lines()[1].GetValue("key2")) == std::string_view{"value2"});
    const auto sortedKeys = result.data.SortedKeys();
    REQUIRE(sortedKeys.size() == 2);
    CHECK(sortedKeys[0] == "key1");
    CHECK(sortedKeys[1] == "key2");
    REQUIRE(result.data.Files().size() == 1);
    CHECK(result.data.Files()[0]->GetPath() == testFile.GetFilePath());
    CHECK(result.data.Files()[0]->GetLineCount() == 3);
    CHECK(result.data.Files()[0]->GetLine(0) == R"({"key1":"value1"})");
    CHECK(result.data.Files()[0]->GetLine(1) == "invalid json");
    CHECK(result.data.Files()[0]->GetLine(2) == R"({"key2":"value2"})");
}

TEST_CASE("Parse file with multiple JSON objects and multiple invalid lines", "[json_parser]")
{
    // Parse will end with first invalid line

    loglib::JsonParser parser;
    TestJsonLogFile testFile(
        {glz::generic_sorted_u64{{"key1", "value1"}},
         "invalid json 1",
         glz::generic_sorted_u64{{"key2", "value2"}},
         "invalid json 2"}
    );

    auto result = parser.Parse(testFile.GetFilePath());
    CHECK(result.errors.size() == 2);
    REQUIRE(result.data.SortedKeys().size() == 2);
    CHECK(loglib::AsStringView(result.data.Lines()[0].GetValue("key1")) == std::string_view{"value1"});
    CHECK(loglib::AsStringView(result.data.Lines()[1].GetValue("key2")) == std::string_view{"value2"});
    REQUIRE(result.data.Files().size() == 1);
    REQUIRE(result.data.Lines().size() == 2);
    CHECK(result.data.Files()[0]->GetPath() == testFile.GetFilePath());
    CHECK(result.data.Files()[0]->GetLineCount() == 4);
    CHECK(result.data.Files()[0]->GetLine(0) == R"({"key1":"value1"})");
    CHECK(result.data.Files()[0]->GetLine(1) == "invalid json 1");
    CHECK(result.data.Files()[0]->GetLine(2) == R"({"key2":"value2"})");
    CHECK(result.data.Files()[0]->GetLine(3) == "invalid json 2");
}

TEST_CASE("Convert empty values to string", "[json_parser]")
{
    loglib::JsonParser parser;
    loglib::LogMap values;
    CHECK(parser.ToString(values) == "{}");
}

TEST_CASE("Convert one value to string", "[json_parser]")
{
    loglib::JsonParser parser;

    SECTION("Null")
    {
        loglib::LogMap values{{"key", std::monostate()}};
        CHECK(parser.ToString(values) == R"({"key":null})");
    }

    SECTION("String")
    {
        loglib::LogMap values{{"key", std::string("value")}};
        CHECK(parser.ToString(values) == R"({"key":"value"})");
    }

    SECTION("Unsigned integer")
    {
        loglib::LogMap values{{"key", uint64_t(42)}};
        CHECK(parser.ToString(values) == R"({"key":42})");
    }

    SECTION("Integer")
    {
        loglib::LogMap values{{"key", int64_t(-12)}};
        CHECK(parser.ToString(values) == R"({"key":-12})");
    }

    SECTION("Double")
    {
        loglib::LogMap values{{"key", 3.14}};
        CHECK(parser.ToString(values) == R"({"key":3.14})");
    }

    SECTION("Boolean")
    {
        loglib::LogMap values{{"key", true}};
        CHECK(parser.ToString(values) == R"({"key":true})");
    }
}

TEST_CASE("Convert all possible values to string", "[json_parser]")
{
    loglib::JsonParser parser;
    loglib::LogMap values{
        {"null", std::monostate()},
        {"string", std::string("value")},
        {"uinteger", uint64_t(42)},
        {"integer", int64_t(-12)},
        {"double", 3.14},
        {"boolean", true}
    };

    CHECK(
        parser.ToString(values) ==
        R"({"boolean":true,"double":3.14,"integer":-12,"null":null,"string":"value","uinteger":42})"
    );
}

TEST_CASE("Parallel parse parity vs. single-thread", "[json_parser][parity]")
{
    // Drives the same fixture through the streaming pipeline twice — once with one Stage B
    // worker and once with `hardware_concurrency` workers — and asserts byte-equivalent output
    // (PRD req. 4.1.16, S7). Specifically: same line count, same SortedKeys set, and per-cell
    // value equivalence under `LogValueEquivalent` (which folds string/string_view distinctions
    // into a single string-bytes comparison).
    using namespace loglib;

    JsonParser parser;

    // Generate a fixture sized to span multiple Stage A batches (1 MiB default) so the
    // multi-threaded run actually exercises more than one worker. ~5'000 lines × ~200 bytes
    // each ≈ 1 MB, large enough to split.
    std::vector<TestJsonLogFile::Line> logs;
    logs.reserve(5'000);
    std::mt19937 rng(0xC0FFEE);
    std::uniform_int_distribution<int> levelDist(0, 4);
    std::uniform_int_distribution<int> intDist(-1'000, 1'000);
    static constexpr std::array<const char *, 5> kLevels = {"trace", "debug", "info", "warning", "error"};
    for (size_t i = 0; i < 5'000; ++i)
    {
        glz::generic_sorted_u64 json;
        json["index"] = static_cast<int64_t>(i);
        json["level"] = std::string(kLevels[levelDist(rng)]);
        json["component"] = std::string("component_") + std::to_string(i % 7);
        json["message"] = std::string("event ") + std::to_string(i) + " — value " + std::to_string(intDist(rng));
        json["counter"] = static_cast<int64_t>(intDist(rng));
        logs.emplace_back(json);
    }
    const TestJsonLogFile testFile(logs);

    JsonParserOptions singleThread;
    singleThread.threads = 1;

    JsonParserOptions multiThread;
    multiThread.threads = std::max(2u, std::thread::hardware_concurrency());

    auto singleResult = parser.Parse(testFile.GetFilePath(), singleThread);
    auto multiResult = parser.Parse(testFile.GetFilePath(), multiThread);

    REQUIRE(singleResult.errors.empty());
    REQUIRE(multiResult.errors.empty());
    REQUIRE(singleResult.data.Lines().size() == multiResult.data.Lines().size());

    const auto singleKeys = singleResult.data.SortedKeys();
    const auto multiKeys = multiResult.data.SortedKeys();
    REQUIRE(singleKeys == multiKeys);

    // Walk both line vectors in lockstep. Each line stores values sorted by KeyId, but the
    // KeyId assignment is a function of insertion order — which can differ between runs because
    // Stage B workers race on `KeyIndex::GetOrInsert`. So compare via the string key surface,
    // not via positional pair equality on `IndexedValues()`.
    const size_t lineCount = singleResult.data.Lines().size();
    for (size_t row = 0; row < lineCount; ++row)
    {
        const auto &lhs = singleResult.data.Lines()[row];
        const auto &rhs = multiResult.data.Lines()[row];
        for (const auto &key : singleKeys)
        {
            const auto lv = lhs.GetValue(key);
            const auto rv = rhs.GetValue(key);
            INFO("row=" << row << " key=" << key);
            CHECK(LogValueEquivalent(lv, rv));
        }
    }
}
