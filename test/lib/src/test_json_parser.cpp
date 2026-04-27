#include "common.hpp"

#include "buffering_sink.hpp"

#include <loglib/internal/parser_options.hpp>
#include <loglib/json_parser.hpp>
#include <loglib/key_index.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_file.hpp>
#include <loglib/log_line.hpp>
#include <loglib/log_parser.hpp>
#include <loglib/log_processing.hpp>
#include <loglib/parser_options.hpp>

#include <catch2/catch_all.hpp>
#include <date/date.h>
#include <glaze/glaze.hpp>
#include <tsl/robin_map.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>

namespace
{
constexpr uint64_t LARGE_UINT = 10000000000000000000ULL;

// Drives a synchronous parse against the streaming pipeline for tests that
// need to dial advanced tuning knobs but still consume a `ParseResult`.
loglib::ParseResult ParseWithSink(
    const loglib::JsonParser &parser,
    const std::filesystem::path &path,
    const loglib::ParserOptions &options = {},
    const loglib::internal::AdvancedParserOptions &advanced = {}
)
{
    auto logFile = std::make_unique<loglib::LogFile>(path);
    loglib::LogFile *logFilePtr = logFile.get();
    loglib::BufferingSink sink(std::move(logFile));
    parser.ParseStreaming(*logFilePtr, sink, options, advanced);
    loglib::LogData data = sink.TakeData();
    std::vector<std::string> errors = sink.TakeErrors();
    return loglib::ParseResult{std::move(data), std::move(errors)};
}
} // namespace

TEST_CASE(
    "ParserOptions{} + AdvancedParserOptions{} reproduces the legacy default-options behaviour",
    "[json_parser][parser_options][defaults]"
)
{
    using namespace loglib;

    const ParserOptions defaults;
    CHECK_FALSE(defaults.stopToken.stop_possible());
    CHECK(defaults.configuration == nullptr);

    const internal::AdvancedParserOptions advanced;
    CHECK(advanced.threads == 0u);
    CHECK(advanced.batchSizeBytes == internal::AdvancedParserOptions::kDefaultBatchSizeBytes);
    CHECK(advanced.ntokens == 0u);
    CHECK(advanced.useThreadLocalKeyCache == true);
    CHECK(advanced.timings == nullptr);
    CHECK(internal::AdvancedParserOptions::kDefaultMaxThreads == 8u);
    CHECK(internal::AdvancedParserOptions::kDefaultBatchSizeBytes == 1024u * 1024u);
}

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
        // Unescaped string values are emitted as `std::string_view` into the mmap;
        // use `AsStringView` so the test is agnostic to which alternative the parser
        // picks per the fast/slow path heuristic.
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
    // Drives the same fixture through the streaming pipeline twice — once with
    // a single worker and once with `hardware_concurrency` workers — and
    // asserts byte-equivalent output: same line count, same `SortedKeys` set,
    // and per-cell value equivalence under `LogValueEquivalent` (which folds
    // `string` / `string_view` distinctions into one bytes comparison).
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

    ParserOptions opts;
    internal::AdvancedParserOptions singleThread;
    singleThread.threads = 1;

    internal::AdvancedParserOptions multiThread;
    multiThread.threads = std::max(2u, std::thread::hardware_concurrency());

    auto singleResult = ParseWithSink(parser, testFile.GetFilePath(), opts, singleThread);
    auto multiResult = ParseWithSink(parser, testFile.GetFilePath(), opts, multiThread);

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

#ifdef LOGLIB_KEY_INDEX_INSTRUMENTATION

TEST_CASE(
    "Per-worker key cache eats canonical KeyIndex::GetOrInsert calls on the hot path",
    "[json_parser][per_worker_cache]"
)
{
    // With `useThreadLocalKeyCache = true`, a 100-line stream that uses only
    // 5 fixed keys must leave `KeyIndex::LoadGetOrInsertCount() <=
    // effectiveThreads × 5`: the canonical lookup fires exactly once per
    // (worker × key) pair on first sight, then every subsequent field key
    // for that worker hits the per-worker `tsl::robin_map` cache instead.
    //
    // The complementary check (cache off → call count rises toward `lines ×
    // keys` = 500) keeps the instrumentation honest: if the macro silently
    // no-ops or the counter never increments, both halves of the test
    // would pass trivially. A hard floor of 100 on the cache-off run
    // catches that failure mode.
    using namespace loglib;

    constexpr size_t kLineCount = 100;
    constexpr unsigned int kThreads = 4;

    // Build a fixture where every line carries the same 5 keys; the values vary so the lines
    // are not byte-identical (avoids any hypothetical dedup short-circuit) but the key surface
    // is locked to exactly {key1..key5}.
    std::vector<TestJsonLogFile::Line> lines;
    lines.reserve(kLineCount);
    for (size_t i = 0; i < kLineCount; ++i)
    {
        glz::generic_sorted_u64 json;
        json["key1"] = static_cast<int64_t>(i);
        json["key2"] = std::string("value_") + std::to_string(i);
        json["key3"] = static_cast<int64_t>(i * 2);
        json["key4"] = (i % 2 == 0);
        json["key5"] = static_cast<double>(i) * 0.5;
        lines.emplace_back(json);
    }
    const TestJsonLogFile testFile(lines);

    JsonParser parser;
    ParserOptions opts;
    internal::AdvancedParserOptions advanced;
    advanced.threads = kThreads;
    advanced.useThreadLocalKeyCache = true;

    KeyIndex::ResetInstrumentationCounters();
    const auto cachedResult = ParseWithSink(parser, testFile.GetFilePath(), opts, advanced);
    const std::size_t cachedCalls = KeyIndex::LoadGetOrInsertCount();

    REQUIRE(cachedResult.errors.empty());
    REQUIRE(cachedResult.data.Lines().size() == kLineCount);

    // Upper bound: every Stage B worker may pay one canonical lookup per fresh key before its
    // local cache is populated. effectiveThreads × keys is the worst case; the actual number is
    // typically smaller because not every worker runs and not every worker sees every key.
    INFO("cached run cachedCalls=" << cachedCalls << " bound=" << (kThreads * 5));
    CHECK(cachedCalls <= static_cast<std::size_t>(kThreads) * 5);

    // Sanity: at least the keys themselves were registered exactly once per unique key.
    CHECK(cachedResult.data.SortedKeys().size() == 5);

    // Now flip the cache off and re-parse. Every field-key sighting must now traverse the
    // canonical KeyIndex, so the counter should sit close to lines × keys (500 on this fixture).
    internal::AdvancedParserOptions noCacheAdvanced = advanced;
    noCacheAdvanced.useThreadLocalKeyCache = false;

    KeyIndex::ResetInstrumentationCounters();
    const auto noCacheResult = ParseWithSink(parser, testFile.GetFilePath(), opts, noCacheAdvanced);
    const std::size_t noCacheCalls = KeyIndex::LoadGetOrInsertCount();

    REQUIRE(noCacheResult.errors.empty());
    REQUIRE(noCacheResult.data.Lines().size() == kLineCount);

    INFO(
        "cache-off run noCacheCalls=" << noCacheCalls
                                      << " expected close to lines*keys=" << (kLineCount * 5)
    );
    // Floor at 100 is comfortably above the cache-on bound (kThreads × 5 = 20) so the check
    // catches a silently-stuck instrumentation counter as well as a regressed cache-off path.
    CHECK(noCacheCalls >= 100);
    // Hard upper bound: there is one GetOrInsert per (line × key) pair plus the worst-case
    // dedup pass, which is bounded by the same number again. 2 × lines × keys is the loosest
    // honest cap that still flags an unexpected explosion.
    CHECK(noCacheCalls <= 2 * kLineCount * 5);
}

#endif // LOGLIB_KEY_INDEX_INSTRUMENTATION

namespace
{

// Local replicas of the transparent-hash adapters from `library/src/json_parser.cpp` so the
// move-survival test can stand up the exact `tsl::robin_map<std::string, KeyId, ..., ...>`
// instantiation that backs `loglib::detail::PerWorkerKeyCache`. The wrapper struct in
// `json_parser.cpp` adds nothing beyond the map field, so its compiler-generated move
// constructor delegates straight through to the map's move constructor — exactly what we
// exercise here.
struct TestTransparentStringHash
{
    using is_transparent = void;

    size_t operator()(std::string_view sv) const noexcept
    {
        return std::hash<std::string_view>{}(sv);
    }
    size_t operator()(const std::string &s) const noexcept
    {
        return std::hash<std::string_view>{}(s);
    }
    size_t operator()(const char *s) const noexcept
    {
        return std::hash<std::string_view>{}(std::string_view(s));
    }
};

struct TestTransparentStringEqual
{
    using is_transparent = void;

    bool operator()(std::string_view lhs, std::string_view rhs) const noexcept
    {
        return lhs == rhs;
    }
    bool operator()(std::string_view lhs, const std::string &rhs) const noexcept
    {
        return lhs == rhs;
    }
    bool operator()(const std::string &lhs, std::string_view rhs) const noexcept
    {
        return lhs == rhs;
    }
    bool operator()(const std::string &lhs, const std::string &rhs) const noexcept
    {
        return lhs == rhs;
    }
};

using TestPerWorkerKeyCache = tsl::robin_map<std::string, loglib::KeyId, TestTransparentStringHash,
                                             TestTransparentStringEqual>;

} // namespace

TEST_CASE("Per-worker key cache survives move construction", "[json_parser][key_cache][move]")
{
    // `tbb::enumerable_thread_specific<WorkerState>` may move-construct worker
    // slots when its per-thread table grows. If `PerWorkerKeyCache`'s move
    // constructor ever drops cached entries (e.g. someone replaces the
    // underlying container with a non-move-preserving one), every worker
    // silently regresses to unbounded canonical `KeyIndex::GetOrInsert`
    // calls. The cache-hit test above would catch that — but only after a
    // full pipeline run. This test pins the contract directly on the cache
    // type so a future container swap fails fast at the unit-test tier.
    using namespace loglib;

    TestPerWorkerKeyCache source;
    source.emplace("alpha", static_cast<KeyId>(7));
    source.emplace("beta", static_cast<KeyId>(11));
    source.emplace("gamma", static_cast<KeyId>(13));

    REQUIRE(source.size() == 3);

    // Move-construct: this is what `enumerable_thread_specific` does when growing its
    // per-thread slot table.
    TestPerWorkerKeyCache moved(std::move(source));

    REQUIRE(moved.size() == 3);
    REQUIRE(moved.find(std::string_view{"alpha"}) != moved.end());
    REQUIRE(moved.find(std::string_view{"beta"}) != moved.end());
    REQUIRE(moved.find(std::string_view{"gamma"}) != moved.end());
    CHECK(moved.find(std::string_view{"alpha"})->second == static_cast<KeyId>(7));
    CHECK(moved.find(std::string_view{"beta"})->second == static_cast<KeyId>(11));
    CHECK(moved.find(std::string_view{"gamma"})->second == static_cast<KeyId>(13));

    // Move-assignment too: `enumerable_thread_specific` also reassigns slots in some growth
    // strategies. Reuse `moved` as the source so the original test inputs are still in play.
    TestPerWorkerKeyCache assigned;
    assigned = std::move(moved);

    REQUIRE(assigned.size() == 3);
    CHECK(assigned.find(std::string_view{"alpha"})->second == static_cast<KeyId>(7));
    CHECK(assigned.find(std::string_view{"beta"})->second == static_cast<KeyId>(11));
    CHECK(assigned.find(std::string_view{"gamma"})->second == static_cast<KeyId>(13));

    // Heterogeneous lookup invariant after the move: `find(std::string_view)` resolves the
    // exact same bucket the originating `emplace(std::string)` populated. If the transparent
    // hash adapter is ever broken (e.g. someone routes the `std::string` overload through a
    // different std::hash instantiation than the `std::string_view` overload), this catches it.
    char rawKey[] = {'a', 'l', 'p', 'h', 'a'};
    const std::string_view view(rawKey, sizeof(rawKey));
    auto it = assigned.find(view);
    REQUIRE(it != assigned.end());
    CHECK(it->second == static_cast<KeyId>(7));
}

namespace
{

// Minimal `%FT%T`-style ISO-8601 timestamp generator used by the Stage B promotion test
// below. The `GenerateRandomJsonLogs` helper in `benchmark_json.cpp` does the same job for
// the benchmarks but lives in a different translation unit, so we re-roll a small variant
// here to keep the unit-test side independent.
std::string FormatIsoTimestamp(std::chrono::system_clock::time_point tp)
{
    return date::format("%FT%T", date::floor<std::chrono::milliseconds>(tp));
}

} // namespace

TEST_CASE(
    "Stage B promotes Type::time column values to TimeStamp inline when configuration is supplied",
    "[json_parser][stage_b_timestamps]"
)
{
    // When `ParserOptions::configuration` describes a `Type::time` column,
    // every parsed line whose value at the column's key is a parseable
    // ISO-8601 string must come out of `JsonParser::Parse` already promoted
    // to `TimeStamp`. The legacy whole-data `ParseTimestamps` pass is thus
    // redundant for these snapshot keys, which backs the truthful
    // `LogData::MarkTimestampsParsed()` flag set by `BeginStreaming`.
    using namespace loglib;

    InitializeTimezoneData();

    constexpr size_t kLineCount = 1'000;

    // Build 1 000 lines spaced 1 ms apart so each line carries a distinct timestamp string.
    // Mixing fields per line keeps the per-key type cache from short-circuiting in a way
    // that could mask a regression in the promotion path.
    const auto base = std::chrono::system_clock::now();
    std::vector<TestJsonLogFile::Line> lines;
    lines.reserve(kLineCount);
    for (size_t i = 0; i < kLineCount; ++i)
    {
        glz::generic_sorted_u64 json;
        json["timestamp"] = FormatIsoTimestamp(base + std::chrono::milliseconds(static_cast<int64_t>(i)));
        json["level"] = std::string("info");
        json["thread_id"] = static_cast<int64_t>(i % 8);
        json["message"] = std::string("hello");
        lines.emplace_back(json);
    }
    const TestJsonLogFile testFile(lines);

    // Configuration mirrors the production timestamp column shape: one key
    // (`timestamp`) and one parse format (`%FT%T`). Stage B pre-resolves the
    // key into a `KeyId` at pipeline start and the per-worker
    // `lastValidTimestamp` cache collapses the per-line work to one
    // `date::parse` call after the first sighting.
    auto configuration = std::make_shared<LogConfiguration>();
    LogConfiguration::Column timestampColumn;
    timestampColumn.header = "timestamp";
    timestampColumn.keys = {"timestamp"};
    timestampColumn.type = LogConfiguration::Type::time;
    timestampColumn.parseFormats = {"%FT%T"};
    configuration->columns.push_back(std::move(timestampColumn));

    ParserOptions opts;
    opts.configuration = configuration;

    JsonParser parser;
    const ParseResult result = ParseWithSink(parser, testFile.GetFilePath(), opts);

    REQUIRE(result.errors.empty());
    REQUIRE(result.data.Lines().size() == kLineCount);

    const KeyId timestampKeyId = result.data.Keys().Find("timestamp");
    REQUIRE(timestampKeyId != kInvalidKeyId);

    // Every line's `timestamp` value must be a fully-promoted `TimeStamp` (no leftover
    // strings that the GUI back-fill loop would have to take a second pass on for
    // snapshot-time keys). A single non-TimeStamp slip indicates Stage B did not run the
    // promotion or the per-worker `lastValidTimestamp` cache lost its way mid-batch.
    size_t promoted = 0;
    for (const auto &line : result.data.Lines())
    {
        const LogValue value = line.GetValue(timestampKeyId);
        if (std::holds_alternative<TimeStamp>(value))
        {
            ++promoted;
        }
    }
    INFO("promoted=" << promoted << " of " << kLineCount);
    CHECK(promoted == kLineCount);
}

TEST_CASE(
    "Stage B leaves unparseable timestamp values as strings and pushes no errors",
    "[json_parser][stage_b_timestamps]"
)
{
    // Promotion failures must never push into `parsed.errors` — they leave
    // the value as the original string so the back-fill pass can take a
    // second crack at it. This test pins that contract: a fixture mixing
    // parseable and unparseable timestamp strings must come out with
    // `errors.empty()` and the bad rows still carrying a string at the
    // timestamp KeyId.
    using namespace loglib;

    InitializeTimezoneData();

    std::vector<TestJsonLogFile::Line> lines;
    lines.emplace_back(R"({"timestamp": "2024-01-01T12:00:00", "msg": "ok-1"})");
    lines.emplace_back(R"({"timestamp": "not-a-timestamp", "msg": "bad-1"})");
    lines.emplace_back(R"({"timestamp": "2024-01-02T12:00:00", "msg": "ok-2"})");
    lines.emplace_back(R"({"timestamp": "", "msg": "empty"})");
    lines.emplace_back(R"({"timestamp": "2024-01-03T12:00:00", "msg": "ok-3"})");
    const TestJsonLogFile testFile(lines);

    auto configuration = std::make_shared<LogConfiguration>();
    LogConfiguration::Column timestampColumn;
    timestampColumn.header = "timestamp";
    timestampColumn.keys = {"timestamp"};
    timestampColumn.type = LogConfiguration::Type::time;
    timestampColumn.parseFormats = {"%FT%T"};
    configuration->columns.push_back(std::move(timestampColumn));

    ParserOptions opts;
    opts.configuration = configuration;

    JsonParser parser;
    const ParseResult result = ParseWithSink(parser, testFile.GetFilePath(), opts);

    // Promotion failures are silent; only genuine JSON-parse errors land in `errors`.
    CHECK(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 5);

    const KeyId timestampKeyId = result.data.Keys().Find("timestamp");
    REQUIRE(timestampKeyId != kInvalidKeyId);

    CHECK(std::holds_alternative<TimeStamp>(result.data.Lines()[0].GetValue(timestampKeyId)));
    CHECK_FALSE(std::holds_alternative<TimeStamp>(result.data.Lines()[1].GetValue(timestampKeyId)));
    CHECK(AsStringView(result.data.Lines()[1].GetValue(timestampKeyId)) == std::string_view{"not-a-timestamp"});
    CHECK(std::holds_alternative<TimeStamp>(result.data.Lines()[2].GetValue(timestampKeyId)));
    CHECK_FALSE(std::holds_alternative<TimeStamp>(result.data.Lines()[3].GetValue(timestampKeyId)));
    CHECK(AsStringView(result.data.Lines()[3].GetValue(timestampKeyId)) == std::string_view{""});
    CHECK(std::holds_alternative<TimeStamp>(result.data.Lines()[4].GetValue(timestampKeyId)));
}

TEST_CASE(
    "Streaming pipeline preserves absolute line numbers across runs of mid-stream empty lines",
    "[json_parser][empty_lines]"
)
{
    // A contiguous run of mid-stream empty lines must not desynchronise the
    // chunker's line counter from the decoder's emitted absolute line
    // numbers, and the per-line offset table must still allocate one slot
    // per empty line (an empty line consumes one line number — see
    // `json_parser.cpp`'s empty-line handling).
    using namespace loglib;

    constexpr size_t kEmptyLineCount = 100;

    // Layout: 5 valid lines, 100 blank lines, 5 valid lines. Absolute line numbers
    // observed by the consumer must be {1..5, 106..110}; total file line count
    // (LogFile::GetLineCount, which counts every consumed source line including the
    // blanks) must be 110.
    std::vector<std::string> raw;
    raw.reserve(5 + kEmptyLineCount + 5);
    for (size_t i = 0; i < 5; ++i)
    {
        raw.emplace_back(std::string(R"({"key":"value-)") + std::to_string(i + 1) + R"("})");
    }
    for (size_t i = 0; i < kEmptyLineCount; ++i)
    {
        raw.emplace_back("");
    }
    for (size_t i = 0; i < 5; ++i)
    {
        raw.emplace_back(std::string(R"({"key":"value-)") + std::to_string(i + 6) + R"("})");
    }

    std::vector<TestJsonLogFile::Line> fixtureLines;
    fixtureLines.reserve(raw.size());
    for (const auto &s : raw)
    {
        fixtureLines.emplace_back(s.c_str());
    }
    const TestJsonLogFile testFile(fixtureLines);

    JsonParser parser;
    const ParseResult result = parser.Parse(testFile.GetFilePath());

    REQUIRE(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 10);
    REQUIRE(result.data.Files().size() == 1);

    // Absolute line numbers: 1..5 then 106..110.
    const std::vector<size_t> expected = {1, 2, 3, 4, 5, 106, 107, 108, 109, 110};
    for (size_t i = 0; i < result.data.Lines().size(); ++i)
    {
        INFO("i=" << i);
        CHECK(result.data.Lines()[i].FileReference().GetLineNumber() == expected[i]);
    }

    // The LogFile-side offset table should record one entry per consumed line including
    // every blank line; otherwise GetLine(absoluteLineNumber) would skew when the GUI later
    // double-clicks a row to open the source line.
    const LogFile &file = *result.data.Files()[0];
    CHECK(file.GetLineCount() == 110);

    // Spot-check that a specific empty-line index round-trips to an empty string and that
    // the surrounding valid lines still render their original JSON content.
    CHECK(file.GetLine(0) == R"({"key":"value-1"})");
    CHECK(file.GetLine(4) == R"({"key":"value-5"})");
    CHECK(file.GetLine(50).empty()); // somewhere in the empty run
    CHECK(file.GetLine(105) == R"({"key":"value-6"})");
    CHECK(file.GetLine(109) == R"({"key":"value-10"})");
}

TEST_CASE(
    "ExtractFieldKey round-trips quoted, escaped, and Unicode-escape keys",
    "[json_parser][extract_field_key]"
)
{
    // `ExtractFieldKey` uses simdjson's length-aware `field.escaped_key()` and
    // keeps the fast/slow-path split intact: keys with no backslash become a
    // `string_view` directly into the input; keys with any backslash byte
    // fall back to `field.unescaped_key()` and an owned `std::string`. The
    // four canonical key shapes below cover every observable code path; a
    // regression would surface as a missing key in the canonical `KeyIndex`.
    using namespace loglib;

    // Each line carries one peculiar key plus a stable value so the assertion can be
    // keyed by line index. The raw-string contents are JSON-on-disk, i.e. backslashes
    // are real characters in the bytes simdjson sees.
    std::vector<TestJsonLogFile::Line> lines;
    lines.emplace_back(R"({"plain": "v-plain"})");          // (a) fast path: no backslash
    lines.emplace_back(R"({"with\"quote": "v-quote"})");    // (b) slow path: escaped quote
    lines.emplace_back(R"({"back\\slash": "v-backslash"})"); // (c) slow path: escaped backslash
    lines.emplace_back(R"({"\u0041BC": "v-unicode"})");      // (d) slow path: Unicode escape
    const TestJsonLogFile testFile(lines);

    JsonParser parser;
    const ParseResult result = parser.Parse(testFile.GetFilePath());
    REQUIRE(result.errors.empty());
    REQUIRE(result.data.Lines().size() == lines.size());

    // Expected unescaped key per line, paired with the value it should reach. The keys
    // are the post-unescape forms simdjson would produce — e.g. \u0041 collapses to A.
    const std::vector<std::pair<std::string, std::string>> expected = {
        {"plain", "v-plain"},
        {"with\"quote", "v-quote"},
        {"back\\slash", "v-backslash"},
        {"ABC", "v-unicode"},
    };

    for (size_t i = 0; i < expected.size(); ++i)
    {
        INFO("i=" << i << " expected key=" << expected[i].first << " value=" << expected[i].second);
        const KeyId keyId = result.data.Keys().Find(expected[i].first);
        REQUIRE(keyId != kInvalidKeyId);
        const LogValue value = result.data.Lines()[i].GetValue(keyId);
        CHECK(AsStringView(value) == std::string_view{expected[i].second});
    }
}

TEST_CASE(
    "Padded-tail slow path parses lines within SIMDJSON_PADDING bytes of EOF",
    "[json_parser][padding_tail]"
)
{
    // Every short fixture's last line ends within `SIMDJSON_PADDING` bytes
    // of EOF, so Stage B's `!sourceIsStable` branch takes over and parses
    // the line out of the per-worker `linePadded` scratch instead of the
    // mmap directly. The slow path uses explicit `memcpy` + `memset` +
    // length-aware `iterate` plus a one-shot `linePadded.resize(...)`
    // sized to the largest line observed by the worker. This test
    // exercises (1) the resize-on-grow branch when later lines are longer
    // than earlier ones, and (2) the memcpy + memset + iterate body
    // itself, which must produce values byte-identical to the fast path.
    using namespace loglib;

    // Three lines whose payloads grow monotonically, so the slow-path resize fires
    // first on line 1, again on line 2, and not on line 3 (capacity already covers it).
    // Padding is only ~64 bytes on Win/MSVC so even the longest line below sits well
    // within SIMDJSON_PADDING of EOF for any reasonable file size.
    std::vector<TestJsonLogFile::Line> lines;
    lines.emplace_back(R"({"k":"a"})");                                  // shortest
    lines.emplace_back(R"({"k":"abcdefghijklmnopqrstuvwx"})");           // longer
    lines.emplace_back(R"({"k":"abcdefghijklmnopqrstuvwxyz0123456789"})"); // longest
    const TestJsonLogFile testFile(lines);

    JsonParser parser;
    const ParseResult result = parser.Parse(testFile.GetFilePath());
    REQUIRE(result.errors.empty());
    REQUIRE(result.data.Lines().size() == lines.size());

    const KeyId keyId = result.data.Keys().Find("k");
    REQUIRE(keyId != kInvalidKeyId);

    const std::vector<std::string> expectedValues = {
        "a",
        "abcdefghijklmnopqrstuvwx",
        "abcdefghijklmnopqrstuvwxyz0123456789",
    };

    for (size_t i = 0; i < expectedValues.size(); ++i)
    {
        INFO("i=" << i << " expected value=" << expectedValues[i]);
        const LogValue value = result.data.Lines()[i].GetValue(keyId);
        CHECK(AsStringView(value) == std::string_view{expectedValues[i]});
    }
}

TEST_CASE(
    "InsertSorted preserves last-write-wins on duplicate keys above the lower_bound threshold",
    "[json_parser][duplicate_keys]"
)
{
    // Above ~8 fields per line `InsertSorted` switches from its narrow-row
    // linear back-scan to `std::lower_bound`. Duplicate-key semantics must
    // remain identical across the threshold: when the same key appears
    // more than once in a single JSON line, the *last* occurrence wins
    // (matching the legacy `LogMap` insert behaviour the rest of the parser
    // relies on).
    //
    // The narrow-row branch handled this by overwriting the equal-keyed slot it
    // discovered while back-scanning. The new lower_bound branch checks the result
    // for an exact-key match before emplacing. This test pins both branches at once
    // by emitting one line that hits each side of the threshold and asserting that
    // the duplicate-key value the parser keeps is the second one in document order.
    using namespace loglib;

    // (1) Wide row: 12 fields total, with `dup` duplicated. The 12 distinct field
    //     positions push `out.size()` above the kInsertSortedLowerBoundThreshold (8)
    //     well before the second `dup` arrives, so the duplicate handling exercises
    //     the lower_bound branch. We hand-craft the JSON so simdjson preserves the
    //     duplicate (`glz::generic_sorted_u64` would dedup at construction time).
    //
    // (2) Narrow row: 4 fields with `dup` duplicated. `out.size() < 8` for the
    //     entire line, so the linear back-scan branch handles it. Both rows must
    //     yield the same "last write wins" outcome.
    std::vector<TestJsonLogFile::Line> lines;
    lines.emplace_back(
        R"({"a":1,"b":2,"c":3,"d":4,"e":5,"f":6,"g":7,"h":8,"i":9,"dup":"first","j":11,"dup":"second"})"
    );
    lines.emplace_back(R"({"a":1,"dup":"first","b":2,"dup":"second"})");

    const TestJsonLogFile testFile(lines);

    JsonParser parser;
    const ParseResult result = parser.Parse(testFile.GetFilePath());
    REQUIRE(result.errors.empty());
    REQUIRE(result.data.Lines().size() == lines.size());

    const KeyId dupKey = result.data.Keys().Find("dup");
    REQUIRE(dupKey != kInvalidKeyId);

    // Wide row exercises the lower_bound branch.
    {
        INFO("wide row (>= 8 fields): lower_bound branch");
        const LogValue value = result.data.Lines()[0].GetValue(dupKey);
        CHECK(AsStringView(value) == std::string_view{"second"});
    }

    // Narrow row exercises the linear back-scan branch.
    {
        INFO("narrow row (< 8 fields): linear back-scan branch");
        const LogValue value = result.data.Lines()[1].GetValue(dupKey);
        CHECK(AsStringView(value) == std::string_view{"second"});
    }
}
