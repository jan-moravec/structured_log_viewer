#include "common.hpp"

#include <loglib/bytes_producer.hpp>
#include <loglib/file_line_source.hpp>
#include <loglib/internal/advanced_parser_options.hpp>
#include <loglib/internal/buffering_sink.hpp>
#include <loglib/key_index.hpp>
#include <loglib/line_source.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_file.hpp>
#include <loglib/log_line.hpp>
#include <loglib/log_parse_sink.hpp>
#include <loglib/log_parser.hpp>
#include <loglib/parse_file.hpp>
#include <loglib/parser_options.hpp>
#include <loglib/parsers/json_parser.hpp>
#include <loglib/stream_line_source.hpp>

#include <catch2/catch_all.hpp>
#include <date/date.h>
#include <glaze/glaze.hpp>
#include <tsl/robin_map.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
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
    const std::filesystem::path &path,
    const loglib::ParserOptions &options = {},
    const loglib::internal::AdvancedParserOptions &advanced = {}
)
{
    auto logFile = std::make_unique<loglib::LogFile>(path);
    auto source = std::make_unique<loglib::FileLineSource>(std::move(logFile));
    loglib::FileLineSource *sourcePtr = source.get();
    loglib::internal::BufferingSink sink(std::move(source));
    loglib::JsonParser::ParseStreaming(*sourcePtr, sink, options, advanced);
    loglib::LogData data = sink.TakeData();
    std::vector<std::string> errors = sink.TakeErrors();
    return loglib::ParseResult{.data = std::move(data), .errors = std::move(errors)};
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
    CHECK(advanced.batchSizeBytes == internal::AdvancedParserOptions::DEFAULT_BATCH_SIZE_BYTES);
    CHECK(internal::AdvancedParserOptions::DEFAULT_MAX_THREADS == 8u);
    CHECK(internal::AdvancedParserOptions::DEFAULT_BATCH_SIZE_BYTES == 1024u * 1024u);
}

TEST_CASE("Validate non-existent file", "[json_parser]")
{
    const loglib::JsonParser parser;
    CHECK_FALSE(parser.IsValid("non_existent_file.json"));
}

TEST_CASE("Validate empty file", "[json_parser]")
{
    const loglib::JsonParser parser;
    const TestJsonLogFile testFile;
    CHECK_FALSE(parser.IsValid(testFile.GetFilePath()));
}

TEST_CASE("Validate file with empty lines", "[json_parser]")
{
    const loglib::JsonParser parser;
    const TestJsonLogFile testFile(TestJsonLogFile::Line("\n\n\n"));
    CHECK_FALSE(parser.IsValid(testFile.GetFilePath()));
}

TEST_CASE("Validate file with leading blank line", "[json_parser]")
{
    // Leading blank lines are tolerated: the first non-empty line is what determines validity.
    const loglib::JsonParser parser;
    const TestJsonLogFile testFile(std::vector<TestJsonLogFile::Line>{"\n", R"({"key": "value"})"});
    CHECK(parser.IsValid(testFile.GetFilePath()));
}

TEST_CASE("Validate file with invalid line", "[json_parser]")
{
    const loglib::JsonParser parser;
    const TestJsonLogFile testFile(TestJsonLogFile::Line("invalid json"));
    CHECK_FALSE(parser.IsValid(testFile.GetFilePath()));
}

TEST_CASE("Validate file with empty JSON object", "[json_parser]")
{
    const loglib::JsonParser parser;
    const TestJsonLogFile testFile(TestJsonLogFile::Line(R"({})"));
    CHECK(parser.IsValid(testFile.GetFilePath()));
}

TEST_CASE("Validate file with JSON line", "[json_parser]")
{
    const loglib::JsonParser parser;
    const TestJsonLogFile testFile(TestJsonLogFile::Line(R"({"key": "value"})"));
    CHECK(parser.IsValid(testFile.GetFilePath()));
}

TEST_CASE("Parse non-existent file", "[json_parser]")
{
    const loglib::JsonParser parser;
    CHECK_THROWS_AS(ParseFile(parser, "non_existent_file.json"), std::runtime_error);
}

TEST_CASE("Parse empty file", "[json_parser]")
{
    const loglib::JsonParser parser;
    const TestJsonLogFile testFile;
    CHECK_THROWS_AS(ParseFile(parser, testFile.GetFilePath()), std::runtime_error);
}

TEST_CASE("Parse file with empty lines", "[json_parser]")
{
    // Blank lines are not errors, just content-free input. Parse should succeed with no data
    // and no errors so callers can distinguish this from parse failures.
    const loglib::JsonParser parser;
    const TestJsonLogFile testFile(TestJsonLogFile::Line("\n\n\n"));
    auto result = ParseFile(parser, testFile.GetFilePath());
    CHECK(result.data.Lines().empty());
    CHECK(result.errors.empty());
}

TEST_CASE("Parse file with invalid line", "[json_parser]")
{
    // A file containing only invalid lines produces an empty LogData but reports each failure
    // through `errors` so the caller can surface them.
    const loglib::JsonParser parser;
    const TestJsonLogFile testFile(TestJsonLogFile::Line("invalid json"));
    auto result = ParseFile(parser, testFile.GetFilePath());
    CHECK(result.data.Lines().empty());
    CHECK(result.errors.size() == 1);
}

TEST_CASE("Parse file with invalid and valid line", "[json_parser]")
{
    const loglib::JsonParser parser;
    const TestJsonLogFile testFile(std::vector<TestJsonLogFile::Line>({"invalid json", R"({"key": "value"})"}));
    auto result = ParseFile(parser, testFile.GetFilePath());
    CHECK(result.errors.size() == 1);
    CHECK(result.data.Lines().size() == 1);
}

TEST_CASE("Parse file with multiple invalid lines", "[json_parser]")
{
    const loglib::JsonParser parser;
    const TestJsonLogFile testFile(std::vector<TestJsonLogFile::Line>{"invalid json 1", "invalid json 2"});
    auto result = ParseFile(parser, testFile.GetFilePath());
    CHECK(result.data.Lines().empty());
    CHECK(result.errors.size() == 2);
}

// Regression: `DecodeJsonBatch` used to format errors with the line number
// *relative to the current Stage A batch*, which resets to 1 at each new
// pipeline batch. For files larger than `batchSizeBytes` this caused every
// error past batch 1 to report the wrong line — e.g. a malformed line at file
// line 1500 surfacing as "Error on line 500" because it was the 500th line of
// the second batch. The fix moves the formatting into Stage C, which knows the
// running absolute line cursor and composes "Error on line N: <body>" itself.
TEST_CASE("Parse file with invalid lines spanning multiple pipeline batches", "[json_parser][error_line_numbers]")
{
    const loglib::JsonParser parser;

    constexpr size_t VALID_LINES = 1000;
    constexpr size_t INVALID_LINE_NUMBER_A = 1500;
    constexpr size_t INVALID_LINE_NUMBER_B = 2500;

    // Build a file with valid JSON sandwiching two malformed lines at known
    // absolute line numbers, well past the first pipeline batch boundary.
    // `JsonLogLine` only has a `const char *` (and a parsed-JSON) constructor,
    // so we materialise each line as `std::string` first and pass `.c_str()`
    // explicitly — passing a `std::string&&` would otherwise silently bind to
    // the parsed-JSON constructor and wrap each entire line as a JSON string
    // scalar, producing "not a JSON object" errors for every row.
    std::vector<std::string> lineTexts;
    lineTexts.reserve(INVALID_LINE_NUMBER_B);
    // Note: the `std::string&`+append pattern is deliberate over
    // `operator+` chains. GCC 13 on the Linux CI runner emits a
    // false-positive `-Wstringop-overread` / `-Wstringop-overflow`
    // on `std::string("prefix ") + std::to_string(i)` when the
    // resulting string leaves the SSO buffer (libstdc++ SSO cap is
    // 15 bytes for char strings); the separate `.append()` calls
    // avoid the temporary that the heuristic misanalyses.
    for (size_t i = 1; i <= INVALID_LINE_NUMBER_B; ++i)
    {
        std::string line;
        if (i == INVALID_LINE_NUMBER_A || i == INVALID_LINE_NUMBER_B)
        {
            line = "not json line ";
            line.append(std::to_string(i));
        }
        else
        {
            line = R"({"index": )";
            line.append(std::to_string(i));
            line.append("}");
        }
        lineTexts.push_back(std::move(line));
    }
    std::vector<TestJsonLogFile::Line> lines;
    lines.reserve(lineTexts.size());
    for (const std::string &text : lineTexts)
    {
        lines.emplace_back(text.c_str());
    }
    static_cast<void>(VALID_LINES);
    const TestJsonLogFile testFile(std::move(lines));

    // Force several Stage A batches by capping `batchSizeBytes` well below
    // the file size; the INVALID_LINE_NUMBER_A line therefore lands in batch 2+.
    const loglib::ParserOptions options;
    loglib::internal::AdvancedParserOptions advanced;
    advanced.batchSizeBytes = 8 * 1024;
    advanced.threads = 1;

    auto result = ParseWithSink(testFile.GetFilePath(), options, advanced);

    REQUIRE(result.errors.size() == 2);
    const std::string expectedA = "Error on line " + std::to_string(INVALID_LINE_NUMBER_A);
    const std::string expectedB = "Error on line " + std::to_string(INVALID_LINE_NUMBER_B);
    CHECK(result.errors[0].contains(expectedA));
    CHECK(result.errors[1].contains(expectedB));
}

TEST_CASE("Parse file with empty JSON object", "[json_parser]")
{
    const loglib::JsonParser parser;
    const TestJsonLogFile testFile(TestJsonLogFile::Line(R"({})"));

    auto result = ParseFile(parser, testFile.GetFilePath());
    CHECK(result.errors.empty());
    REQUIRE(result.data.Lines().size() == testFile.JsonLines().size());
    CHECK(result.data.Lines()[0].IndexedValues().empty());
    CHECK(result.data.Keys().Size() == 0);
    REQUIRE(result.data.Sources().size() == 1);
    CHECK(result.data.FrontFileSource()->File().GetPath() == testFile.GetFilePath());
    CHECK(result.data.FrontFileSource()->File().GetLine(0) == "{}");
}

TEST_CASE("Parse file with multiple empty JSON objects", "[json_parser]")
{
    const loglib::JsonParser parser;
    const TestJsonLogFile testFile(std::vector<TestJsonLogFile::Line>{R"({})", R"({})"});

    auto result = ParseFile(parser, testFile.GetFilePath());
    CHECK(result.errors.empty());
    REQUIRE(result.data.Lines().size() == testFile.JsonLines().size());
    CHECK(result.data.Keys().Size() == 0);
    REQUIRE(result.data.Sources().size() == 1);
    CHECK(result.data.FrontFileSource()->File().GetPath() == testFile.GetFilePath());
    for (size_t i = 0; i < result.data.Lines().size(); i++)
    {
        CHECK(result.data.Lines()[i].IndexedValues().empty());
        CHECK(result.data.FrontFileSource()->File().GetLine(i) == "{}");
    }
}

TEST_CASE("Parse file with single JSON object containing single JSON element", "[json_parser]")
{
    const loglib::JsonParser parser;

    SECTION("Null")
    {
        const TestJsonLogFile testFile(TestJsonLogFile::Line(R"({"key": null})"));
        auto result = ParseFile(parser, testFile.GetFilePath());
        CHECK(result.errors.empty());
        REQUIRE(result.data.Lines().size() == testFile.JsonLines().size());
        CHECK(std::holds_alternative<std::monostate>(result.data.Lines()[0].GetValue("key")));
        REQUIRE(result.data.SortedKeys().size() == 1);
        CHECK(result.data.SortedKeys()[0] == "key");
        REQUIRE(result.data.Sources().size() == 1);
        CHECK(result.data.FrontFileSource()->File().GetPath() == testFile.GetFilePath());
        CHECK(result.data.FrontFileSource()->File().GetLine(0) == R"({"key":null})");
    }

    SECTION("String")
    {
        // Unescaped string values are emitted as `std::string_view` into the mmap;
        // use `AsStringView` so the test is agnostic to which alternative the parser
        // picks per the fast/slow path heuristic.
        const TestJsonLogFile testFile(TestJsonLogFile::Line(R"({"key": "value"})"));
        auto result = ParseFile(parser, testFile.GetFilePath());
        CHECK(result.errors.empty());
        REQUIRE(result.data.Lines().size() == testFile.JsonLines().size());
        CHECK(loglib::AsStringView(result.data.Lines()[0].GetValue("key")) == std::string_view{"value"});
        REQUIRE(result.data.SortedKeys().size() == 1);
        CHECK(result.data.SortedKeys()[0] == "key");
        REQUIRE(result.data.Sources().size() == 1);
        CHECK(result.data.FrontFileSource()->File().GetPath() == testFile.GetFilePath());
        CHECK(result.data.FrontFileSource()->File().GetLine(0) == R"({"key":"value"})");
    }

    SECTION("Unsigned integer")
    {
        const TestJsonLogFile testFile(TestJsonLogFile::Line(R"({"key": 10000000000000000000})"));
        auto result = ParseFile(parser, testFile.GetFilePath());
        CHECK(result.errors.empty());
        REQUIRE(result.data.Lines().size() == testFile.JsonLines().size());
        CHECK(std::get<uint64_t>(result.data.Lines()[0].GetValue("key")) == LARGE_UINT);
        REQUIRE(result.data.SortedKeys().size() == 1);
        CHECK(result.data.SortedKeys()[0] == "key");
        REQUIRE(result.data.Sources().size() == 1);
        CHECK(result.data.FrontFileSource()->File().GetPath() == testFile.GetFilePath());
        CHECK(result.data.FrontFileSource()->File().GetLine(0) == R"({"key":10000000000000000000})");
    }

    SECTION("Integer")
    {
        const TestJsonLogFile testFile(TestJsonLogFile::Line(R"({"key": -12})"));
        auto result = ParseFile(parser, testFile.GetFilePath());
        CHECK(result.errors.empty());
        REQUIRE(result.data.Lines().size() == testFile.JsonLines().size());
        CHECK(std::get<int64_t>(result.data.Lines()[0].GetValue("key")) == -12);
        REQUIRE(result.data.SortedKeys().size() == 1);
        CHECK(result.data.SortedKeys()[0] == "key");
        REQUIRE(result.data.Sources().size() == 1);
        CHECK(result.data.FrontFileSource()->File().GetPath() == testFile.GetFilePath());
        CHECK(result.data.FrontFileSource()->File().GetLine(0) == R"({"key":-12})");
    }

    SECTION("Double")
    {
        const TestJsonLogFile testFile(TestJsonLogFile::Line(R"({"key": 3.14})"));
        auto result = ParseFile(parser, testFile.GetFilePath());
        CHECK(result.errors.empty());
        REQUIRE(result.data.Lines().size() == testFile.JsonLines().size());
        CHECK(std::get<double>(result.data.Lines()[0].GetValue("key")) == Catch::Approx(3.14));
        REQUIRE(result.data.SortedKeys().size() == 1);
        CHECK(result.data.SortedKeys()[0] == "key");
        REQUIRE(result.data.Sources().size() == 1);
        CHECK(result.data.FrontFileSource()->File().GetPath() == testFile.GetFilePath());
        CHECK(result.data.FrontFileSource()->File().GetLine(0) == R"({"key":3.14})");
    }

    SECTION("Boolean")
    {
        const TestJsonLogFile testFile(TestJsonLogFile::Line(R"({"key": true})"));
        auto result = ParseFile(parser, testFile.GetFilePath());
        REQUIRE(result.errors.empty());
        CHECK(result.data.Lines().size() == testFile.JsonLines().size());
        CHECK(std::get<bool>(result.data.Lines()[0].GetValue("key")) == true);
        REQUIRE(result.data.SortedKeys().size() == 1);
        CHECK(result.data.SortedKeys()[0] == "key");
        REQUIRE(result.data.Sources().size() == 1);
        CHECK(result.data.FrontFileSource()->File().GetPath() == testFile.GetFilePath());
        CHECK(result.data.FrontFileSource()->File().GetLine(0) == R"({"key":true})");
    }
}

TEST_CASE("Parse file with single JSON object containing all possible JSON elements", "[json_parser]")
{
    const loglib::JsonParser parser;
    const TestJsonLogFile testFile(glz::generic_sorted_u64{
        {"null", nullptr},
        {"string", "value"},
        {"uinteger", LARGE_UINT},
        {"integer", -12},
        {"double", 3.14},
        {"boolean", true}
    });

    auto result = ParseFile(parser, testFile.GetFilePath());
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
    REQUIRE(result.data.Sources().size() == 1);
    CHECK(result.data.FrontFileSource()->File().GetPath() == testFile.GetFilePath());
    CHECK(
        result.data.FrontFileSource()->File().GetLine(0) ==
        R"({"boolean":true,"double":3.14,"integer":-12,"null":null,"string":"value","uinteger":10000000000000000000})"
    );
}

TEST_CASE("Parse different key types on different lines", "[json_parser]")
{
    // This test should validate caching
    const loglib::JsonParser parser;
    const TestJsonLogFile testFile(
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

    auto result = ParseFile(parser, testFile.GetFilePath());
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

    REQUIRE(result.data.Sources().size() == 1);
    CHECK(result.data.FrontFileSource()->File().GetPath() == testFile.GetFilePath());
}

TEST_CASE("Parse file with multiple JSON objects", "[json_parser]")
{
    const loglib::JsonParser parser;
    const TestJsonLogFile testFile(
        {glz::generic_sorted_u64{{"key1", "value1"}}, glz::generic_sorted_u64{{"key2", "value2"}}}
    );

    auto result = ParseFile(parser, testFile.GetFilePath());
    CHECK(result.errors.empty());
    REQUIRE(result.data.Lines().size() == testFile.JsonLines().size());
    CHECK(loglib::AsStringView(result.data.Lines()[0].GetValue("key1")) == std::string_view{"value1"});
    CHECK(loglib::AsStringView(result.data.Lines()[1].GetValue("key2")) == std::string_view{"value2"});
    const auto sortedKeys = result.data.SortedKeys();
    REQUIRE(sortedKeys.size() == 2);
    CHECK(sortedKeys[0] == "key1");
    CHECK(sortedKeys[1] == "key2");
    REQUIRE(result.data.Sources().size() == 1);
    CHECK(result.data.FrontFileSource()->File().GetPath() == testFile.GetFilePath());
    CHECK(result.data.FrontFileSource()->File().GetLine(0) == R"({"key1":"value1"})");
    CHECK(result.data.FrontFileSource()->File().GetLine(1) == R"({"key2":"value2"})");
}

TEST_CASE("Parse file whose last line lacks a trailing newline", "[json_parser]")
{
    // Regression: `DecodeJsonBatch` used to push `fileSize` as the sentinel
    // offset for an unterminated final line, but `LogFile::GetLine` derives
    // the line length as `stopOffset - startOffset - 1` (the `-1` being the
    // trailing '\n'). Without the compensating `+ 1` the last character of
    // the final line was silently lopped off when round-tripping through
    // `GetLine` -- here the final `}` would have gone missing.
    const TestLogFile testFile;
    testFile.Write("{\"key1\":\"value1\"}\n{\"key2\":\"value2\"}");

    const loglib::JsonParser parser;
    auto result = ParseFile(parser, testFile.GetFilePath());

    CHECK(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 2);
    CHECK(loglib::AsStringView(result.data.Lines()[0].GetValue("key1")) == std::string_view{"value1"});
    CHECK(loglib::AsStringView(result.data.Lines()[1].GetValue("key2")) == std::string_view{"value2"});

    REQUIRE(result.data.Sources().size() == 1);
    const loglib::LogFile &file = result.data.FrontFileSource()->File();
    REQUIRE(file.GetLineCount() == 2);
    CHECK(file.GetLine(0) == R"({"key1":"value1"})");
    CHECK(file.GetLine(1) == R"({"key2":"value2"})");
}

TEST_CASE("Parse file with multiple JSON objects and one invalid line", "[json_parser]")
{
    // Invalid lines are reported as errors but do not abort the parse.
    const loglib::JsonParser parser;
    const TestJsonLogFile testFile(
        {glz::generic_sorted_u64{{"key1", "value1"}}, "invalid json", glz::generic_sorted_u64{{"key2", "value2"}}}
    );

    auto result = ParseFile(parser, testFile.GetFilePath());
    CHECK(result.errors.size() == 1);
    REQUIRE(result.data.Lines().size() == 2);
    CHECK(loglib::AsStringView(result.data.Lines()[0].GetValue("key1")) == std::string_view{"value1"});
    CHECK(loglib::AsStringView(result.data.Lines()[1].GetValue("key2")) == std::string_view{"value2"});
    const auto sortedKeys = result.data.SortedKeys();
    REQUIRE(sortedKeys.size() == 2);
    CHECK(sortedKeys[0] == "key1");
    CHECK(sortedKeys[1] == "key2");
    REQUIRE(result.data.Sources().size() == 1);
    CHECK(result.data.FrontFileSource()->File().GetPath() == testFile.GetFilePath());
    CHECK(result.data.FrontFileSource()->File().GetLineCount() == 3);
    CHECK(result.data.FrontFileSource()->File().GetLine(0) == R"({"key1":"value1"})");
    CHECK(result.data.FrontFileSource()->File().GetLine(1) == "invalid json");
    CHECK(result.data.FrontFileSource()->File().GetLine(2) == R"({"key2":"value2"})");
}

TEST_CASE("Parse file with multiple JSON objects and multiple invalid lines", "[json_parser]")
{
    // Invalid lines accumulate as errors; valid ones still land in the result.
    const loglib::JsonParser parser;
    const TestJsonLogFile testFile(
        {glz::generic_sorted_u64{{"key1", "value1"}},
         "invalid json 1",
         glz::generic_sorted_u64{{"key2", "value2"}},
         "invalid json 2"}
    );

    auto result = ParseFile(parser, testFile.GetFilePath());
    CHECK(result.errors.size() == 2);
    REQUIRE(result.data.SortedKeys().size() == 2);
    CHECK(loglib::AsStringView(result.data.Lines()[0].GetValue("key1")) == std::string_view{"value1"});
    CHECK(loglib::AsStringView(result.data.Lines()[1].GetValue("key2")) == std::string_view{"value2"});
    REQUIRE(result.data.Sources().size() == 1);
    REQUIRE(result.data.Lines().size() == 2);
    CHECK(result.data.FrontFileSource()->File().GetPath() == testFile.GetFilePath());
    CHECK(result.data.FrontFileSource()->File().GetLineCount() == 4);
    CHECK(result.data.FrontFileSource()->File().GetLine(0) == R"({"key1":"value1"})");
    CHECK(result.data.FrontFileSource()->File().GetLine(1) == "invalid json 1");
    CHECK(result.data.FrontFileSource()->File().GetLine(2) == R"({"key2":"value2"})");
    CHECK(result.data.FrontFileSource()->File().GetLine(3) == "invalid json 2");
}

TEST_CASE("Convert empty values to string", "[json_parser]")
{
    const loglib::JsonParser parser;
    const loglib::LogMap values;
    CHECK(parser.ToString(values) == "{}");
}

TEST_CASE("Convert one value to string", "[json_parser]")
{
    const loglib::JsonParser parser;

    SECTION("Null")
    {
        const loglib::LogMap values{{"key", std::monostate()}};
        CHECK(parser.ToString(values) == R"({"key":null})");
    }

    SECTION("String")
    {
        const loglib::LogMap values{{"key", std::string("value")}};
        CHECK(parser.ToString(values) == R"({"key":"value"})");
    }

    SECTION("Unsigned integer")
    {
        const loglib::LogMap values{{"key", static_cast<uint64_t>(42)}};
        CHECK(parser.ToString(values) == R"({"key":42})");
    }

    SECTION("Integer")
    {
        const loglib::LogMap values{{"key", static_cast<int64_t>(-12)}};
        CHECK(parser.ToString(values) == R"({"key":-12})");
    }

    SECTION("Double")
    {
        const loglib::LogMap values{{"key", 3.14}};
        CHECK(parser.ToString(values) == R"({"key":3.14})");
    }

    SECTION("Boolean")
    {
        const loglib::LogMap values{{"key", true}};
        CHECK(parser.ToString(values) == R"({"key":true})");
    }
}

TEST_CASE("Convert all possible values to string", "[json_parser]")
{
    const loglib::JsonParser parser;
    const loglib::LogMap values{
        {"null", std::monostate()},
        {"string", std::string("value")},
        {"uinteger", static_cast<uint64_t>(42)},
        {"integer", static_cast<int64_t>(-12)},
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

    const JsonParser parser;

    // Generate a fixture sized to span multiple Stage A batches (1 MiB default) so the
    // multi-threaded run actually exercises more than one worker. ~5'000 lines × ~200 bytes
    // each ≈ 1 MB, large enough to split.
    std::vector<TestJsonLogFile::Line> logs;
    logs.reserve(5'000);
    // NOLINTNEXTLINE(bugprone-random-generator-seed,cert-msc32-c,cert-msc51-cpp): deterministic fixture for stable
    // assertions.
    std::mt19937 rng(0xC0FFEE);
    std::uniform_int_distribution<int> levelDist(0, 4);
    std::uniform_int_distribution<int> intDist(-1'000, 1'000);
    static constexpr std::array<const char *, 5> LEVELS = {"trace", "debug", "info", "warning", "error"};
    for (size_t i = 0; i < 5'000; ++i)
    {
        glz::generic_sorted_u64 json;
        json["index"] = static_cast<int64_t>(i);
        json["level"] = std::string(LEVELS[levelDist(rng)]);
        json["component"] = std::string("component_") + std::to_string(i % 7);
        json["message"] = std::string("event ") + std::to_string(i) + " — value " + std::to_string(intDist(rng));
        json["counter"] = static_cast<int64_t>(intDist(rng));
        logs.emplace_back(json);
    }
    const TestJsonLogFile testFile(logs);

    const ParserOptions opts;
    internal::AdvancedParserOptions singleThread;
    singleThread.threads = 1;

    internal::AdvancedParserOptions multiThread;
    multiThread.threads = std::max(2u, std::thread::hardware_concurrency());

    auto singleResult = ParseWithSink(testFile.GetFilePath(), opts, singleThread);
    auto multiResult = ParseWithSink(testFile.GetFilePath(), opts, multiThread);

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
    "Per-worker key cache holds canonical KeyIndex::GetOrInsert calls to threads x keys",
    "[json_parser][per_worker_cache]"
)
{
    // 100 lines, 5 fixed keys: the per-worker tsl::robin_map cache means
    // `KeyIndex::LoadGetOrInsertCount() <= effectiveThreads × 5`. A
    // disabled cache would balloon the count toward `lines × keys` = 500.
    using namespace loglib;

    constexpr size_t LINE_COUNT = 100;
    constexpr unsigned int THREADS = 4;

    // Vary values so lines aren't byte-identical, but lock the key surface
    // to {key1..key5}.
    std::vector<TestJsonLogFile::Line> lines;
    lines.reserve(LINE_COUNT);
    for (size_t i = 0; i < LINE_COUNT; ++i)
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

    const JsonParser parser;
    const ParserOptions opts;
    internal::AdvancedParserOptions advanced;
    advanced.threads = THREADS;

    KeyIndex::ResetInstrumentationCounters();
    const auto cachedResult = ParseWithSink(testFile.GetFilePath(), opts, advanced);
    const std::size_t cachedCalls = KeyIndex::LoadGetOrInsertCount();

    REQUIRE(cachedResult.errors.empty());
    REQUIRE(cachedResult.data.Lines().size() == LINE_COUNT);

    // Upper bound: every Stage B worker may pay one canonical lookup per fresh key before its
    // local cache is populated. effectiveThreads × keys is the worst case; the actual number is
    // typically smaller because not every worker runs and not every worker sees every key.
    INFO("cached run cachedCalls=" << cachedCalls << " bound=" << (THREADS * 5));
    CHECK(cachedCalls <= static_cast<std::size_t>(THREADS) * 5);

    // Sanity: at least the keys themselves were registered exactly once per unique key.
    CHECK(cachedResult.data.SortedKeys().size() == 5);
}

#endif // LOGLIB_KEY_INDEX_INSTRUMENTATION

namespace
{

// Local replicas of the transparent-hash adapters from
// `loglib/internal/transparent_string_hash.hpp` so the move-survival test can stand up the
// exact `tsl::robin_map<std::string, KeyId, ..., ...>` instantiation that backs
// `loglib::internal::PerWorkerKeyCache` (defined in `loglib/internal/parse_runtime.hpp`).
// The wrapper struct adds nothing beyond the map field, so its compiler-generated move
// constructor delegates straight through to the map's move constructor — exactly what we
// exercise here.
struct TestTransparentStringHash
{
    // Named requirement; spelling is fixed.
    // NOLINTNEXTLINE(readability-identifier-naming)
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
    // Named requirement; spelling is fixed.
    // NOLINTNEXTLINE(readability-identifier-naming)
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

using TestPerWorkerKeyCache =
    tsl::robin_map<std::string, loglib::KeyId, TestTransparentStringHash, TestTransparentStringEqual>;

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
    REQUIRE(moved.count(std::string_view{"alpha"}) == 1);
    REQUIRE(moved.count(std::string_view{"beta"}) == 1);
    REQUIRE(moved.count(std::string_view{"gamma"}) == 1);
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
// below. The `test_common::GenerateRandomJsonLogs` helper does the same job for the
// benchmarks but lives in a different translation unit, so we re-roll a small variant
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

    constexpr size_t LINE_COUNT = 1'000;

    // Build 1 000 lines spaced 1 ms apart so each line carries a distinct timestamp string.
    // Mixing fields per line keeps the per-key type cache from short-circuiting in a way
    // that could mask a regression in the promotion path.
    const auto base = std::chrono::system_clock::now();
    std::vector<TestJsonLogFile::Line> lines;
    lines.reserve(LINE_COUNT);
    for (size_t i = 0; i < LINE_COUNT; ++i)
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

    const JsonParser parser;
    const ParseResult result = ParseWithSink(testFile.GetFilePath(), opts);

    REQUIRE(result.errors.empty());
    REQUIRE(result.data.Lines().size() == LINE_COUNT);

    const KeyId timestampKeyId = result.data.Keys().Find("timestamp");
    REQUIRE(timestampKeyId != INVALID_KEY_ID);

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
    INFO("promoted=" << promoted << " of " << LINE_COUNT);
    CHECK(promoted == LINE_COUNT);
}

TEST_CASE(
    "Stage B leaves unparsable timestamp values as strings and pushes no errors", "[json_parser][stage_b_timestamps]"
)
{
    // Promotion failures must never push into `parsed.errors` — they leave
    // the value as the original string so the back-fill pass can take a
    // second crack at it. This test pins that contract: a fixture mixing
    // parseable and unparsable timestamp strings must come out with
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

    const JsonParser parser;
    const ParseResult result = ParseWithSink(testFile.GetFilePath(), opts);

    // Promotion failures are silent; only genuine JSON-parse errors land in `errors`.
    CHECK(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 5);

    const KeyId timestampKeyId = result.data.Keys().Find("timestamp");
    REQUIRE(timestampKeyId != INVALID_KEY_ID);

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

    constexpr size_t EMPTY_LINE_COUNT = 100;

    // Layout: 5 valid lines, 100 blank lines, 5 valid lines. Absolute line numbers
    // observed by the consumer are 0-based indices into `LogFile::mLineOffsets`
    // (so `FileLineSource::RawLine()` round-trips the source bytes); after this
    // fixture they must be {0..4, 105..109}. Total file line count
    // (`LogFile::GetLineCount`, which counts every consumed source line including
    // the blanks) must be 110.
    std::vector<std::string> raw;
    raw.reserve(5 + EMPTY_LINE_COUNT + 5);
    for (size_t i = 0; i < 5; ++i)
    {
        raw.emplace_back(std::string(R"({"key":"value-)") + std::to_string(i + 1) + R"("})");
    }
    for (size_t i = 0; i < EMPTY_LINE_COUNT; ++i)
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

    const JsonParser parser;
    const ParseResult result = ParseFile(parser, testFile.GetFilePath());

    REQUIRE(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 10);
    REQUIRE(result.data.Sources().size() == 1);

    // Absolute (0-based) line numbers: 0..4 then 105..109.
    const std::vector<size_t> expected = {0, 1, 2, 3, 4, 105, 106, 107, 108, 109};
    for (size_t i = 0; i < result.data.Lines().size(); ++i)
    {
        INFO("i=" << i);
        CHECK(result.data.Lines()[i].LineId() == expected[i]);
    }

    // RawLine() must round-trip the source bytes for both the pre-empty-run and
    // post-empty-run regions. This is the contract that motivated the 0-based
    // shift above: prior versions stamped 1-based numbers, so calling GetLine()
    // on the last line threw `out_of_range` and earlier lines returned the next
    // physical line's bytes.
    for (size_t i = 0; i < result.data.Lines().size(); ++i)
    {
        INFO("i=" << i);
        const LogLine &line = result.data.Lines()[i];
        const std::string expectedContent = std::string(R"({"key":"value-)") + std::to_string(i + 1) + R"("})";
        CHECK(line.Source()->RawLine(line.LineId()) == expectedContent);
    }

    // The LogFile-side offset table should record one entry per consumed line including
    // every blank line; otherwise GetLine(absoluteLineNumber) would skew when the GUI later
    // double-clicks a row to open the source line.
    const LogFile &file = result.data.FrontFileSource()->File();
    CHECK(file.GetLineCount() == 110);

    // Spot-check that a specific empty-line index round-trips to an empty string and that
    // the surrounding valid lines still render their original JSON content.
    CHECK(file.GetLine(0) == R"({"key":"value-1"})");
    CHECK(file.GetLine(4) == R"({"key":"value-5"})");
    CHECK(file.GetLine(50).empty()); // somewhere in the empty run
    CHECK(file.GetLine(105) == R"({"key":"value-6"})");
    CHECK(file.GetLine(109) == R"({"key":"value-10"})");
}

TEST_CASE("ExtractFieldKey round-trips quoted, escaped, and Unicode-escape keys", "[json_parser][extract_field_key]")
{
    // Fast path (no backslash): view into source. Slow path (any backslash):
    // owned `std::string` via `field.unescaped_key()`. The four shapes below
    // cover every observable code path.
    using namespace loglib;

    std::vector<TestJsonLogFile::Line> lines;
    lines.emplace_back(R"({"plain": "v-plain"})");           // (a) fast path: no backslash
    lines.emplace_back(R"({"with\"quote": "v-quote"})");     // (b) slow path: escaped quote
    lines.emplace_back(R"({"back\\slash": "v-backslash"})"); // (c) slow path: escaped backslash
    lines.emplace_back(R"({"\u0041BC": "v-unicode"})");      // (d) slow path: Unicode escape
    const TestJsonLogFile testFile(lines);

    const JsonParser parser;
    const ParseResult result = ParseFile(parser, testFile.GetFilePath());
    REQUIRE(result.errors.empty());
    REQUIRE(result.data.Lines().size() == lines.size());

    // Post-unescape form simdjson produces (e.g. \u0041 collapses to A).
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
        REQUIRE(keyId != INVALID_KEY_ID);
        const LogValue value = result.data.Lines()[i].GetValue(keyId);
        CHECK(AsStringView(value) == std::string_view{expected[i].second});
    }
}

TEST_CASE("Padded-tail slow path parses lines within SIMDJSON_PADDING bytes of EOF", "[json_parser][padding_tail]")
{
    // Lines within `SIMDJSON_PADDING` of EOF go through the per-worker
    // `linePadded` scratch instead of aliasing the mmap. Three monotonically
    // growing payloads exercise both the resize-on-grow and the
    // memcpy+memset+iterate body, which must match the fast path bit-for-bit.
    using namespace loglib;

    std::vector<TestJsonLogFile::Line> lines;
    lines.emplace_back(R"({"k":"a"})");                                    // shortest
    lines.emplace_back(R"({"k":"abcdefghijklmnopqrstuvwx"})");             // longer
    lines.emplace_back(R"({"k":"abcdefghijklmnopqrstuvwxyz0123456789"})"); // longest
    const TestJsonLogFile testFile(lines);

    const JsonParser parser;
    const ParseResult result = ParseFile(parser, testFile.GetFilePath());
    REQUIRE(result.errors.empty());
    REQUIRE(result.data.Lines().size() == lines.size());

    const KeyId keyId = result.data.Keys().Find("k");
    REQUIRE(keyId != INVALID_KEY_ID);

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
    // Above ~8 fields `InsertSorted` switches from linear back-scan to
    // `std::lower_bound`. Last-write-wins must hold across the threshold;
    // one wide and one narrow row pin both branches.
    using namespace loglib;

    // (1) Wide row: 12 fields with `dup` duplicated, exercising the
    //     lower_bound branch (INSERT_SORTED_LOWER_BOUND_THRESHOLD is 8).
    //     Hand-craft the JSON so simdjson preserves the duplicate
    //     (`glz::generic_sorted_u64` would dedup at construction time).
    //
    // (2) Narrow row: 4 fields with `dup` duplicated. `out.size() < 8` for the
    //     entire line, so the linear back-scan branch handles it. Both rows must
    //     yield the same "last write wins" outcome.
    std::vector<TestJsonLogFile::Line> lines;
    lines.emplace_back(R"({"a":1,"b":2,"c":3,"d":4,"e":5,"f":6,"g":7,"h":8,"i":9,"dup":"first","j":11,"dup":"second"})"
    );
    lines.emplace_back(R"({"a":1,"dup":"first","b":2,"dup":"second"})");

    const TestJsonLogFile testFile(lines);

    const JsonParser parser;
    const ParseResult result = ParseFile(parser, testFile.GetFilePath());
    REQUIRE(result.errors.empty());
    REQUIRE(result.data.Lines().size() == lines.size());

    const KeyId dupKey = result.data.Keys().Find("dup");
    REQUIRE(dupKey != INVALID_KEY_ID);

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

namespace
{

/// Single-shot in-memory `BytesProducer` for the `StreamLineSource`-based
/// streaming-parser test. Yields the test's pre-baked bytes once and
/// then reports terminal EOF so the parser exits its drain loop
/// without parking on `WaitForBytes`.
class InMemoryProducer final : public loglib::BytesProducer
{
public:
    explicit InMemoryProducer(std::string bytes)
        : mBytes(std::move(bytes))
    {
    }

    size_t Read(std::span<char> buffer) override
    {
        if (mCursor >= mBytes.size())
        {
            mClosed = true;
            return 0;
        }
        const size_t available = mBytes.size() - mCursor;
        const size_t n = std::min(available, buffer.size());
        std::memcpy(buffer.data(), mBytes.data() + mCursor, n);
        mCursor += n;
        if (mCursor >= mBytes.size())
        {
            mClosed = true;
        }
        return n;
    }

    void WaitForBytes(std::chrono::milliseconds /*timeout*/) override
    {
    }

    void Stop() noexcept override
    {
        mClosed = true;
    }

    [[nodiscard]] bool IsClosed() const noexcept override
    {
        return mClosed;
    }

    [[nodiscard]] std::string DisplayName() const override
    {
        return "in-memory";
    }

private:
    std::string mBytes;
    size_t mCursor = 0;
    bool mClosed = false;
};

/// Captures `OnBatch` deliveries for the streaming-parser unit test
/// without depending on Qt. Owns the `KeyIndex` (parser interns keys
/// here) and the move-collected `StreamedBatch`es so the test can
/// inspect emitted `LogLine`s after `OnFinished`.
struct CollectingStreamSink final : loglib::LogParseSink
{
    loglib::KeyIndex keys;
    std::vector<loglib::StreamedBatch> batches;
    bool finished = false;
    bool finishedCancelled = false;

    loglib::KeyIndex &Keys() override
    {
        return keys;
    }
    void OnStarted() override
    {
    }
    void OnBatch(loglib::StreamedBatch batch) override
    {
        batches.push_back(std::move(batch));
    }
    void OnFinished(bool cancelled) override
    {
        finished = true;
        finishedCancelled = cancelled;
    }
};

} // namespace

TEST_CASE(
    "JsonParser::ParseStreaming(StreamLineSource&) emits LogLines tagged with the source",
    "[json_parser][stream_line_source]"
)
{
    using namespace loglib;

    const std::string payload = R"({"k":"hello","n":1})"
                                "\n"
                                R"({"k":"world","n":2})"
                                "\n"
                                R"({"k":"three","n":3})"
                                "\n";

    StreamLineSource source(std::filesystem::path("memory.log"), std::make_unique<InMemoryProducer>(payload));

    CollectingStreamSink sink;
    const JsonParser parser;
    parser.ParseStreaming(source, sink, ParserOptions{});

    REQUIRE(sink.finished);
    CHECK_FALSE(sink.finishedCancelled);

    // Collect every LogLine across the (potentially-coalesced) batches.
    std::vector<LogLine *> lines;
    for (auto &batch : sink.batches)
    {
        for (auto &line : batch.lines)
        {
            lines.push_back(&line);
        }
    }
    REQUIRE(lines.size() == 3);

    // Every emitted LogLine references the source we passed in, with a
    // 1-based monotonic line id.
    for (size_t i = 0; i < lines.size(); ++i)
    {
        CHECK(lines[i]->Source() == &source);
        CHECK(lines[i]->LineId() == i + 1);
    }

    // String values resolve through the source's per-line owned arena.
    const KeyId kKey = sink.keys.Find("k");
    REQUIRE(kKey != INVALID_KEY_ID);
    CHECK(AsStringView(lines[0]->GetValue(kKey)) == std::string_view{"hello"});
    CHECK(AsStringView(lines[1]->GetValue(kKey)) == std::string_view{"world"});
    CHECK(AsStringView(lines[2]->GetValue(kKey)) == std::string_view{"three"});

    // Numeric values round-trip too, via the hot-path compact-storage path.
    const KeyId nKey = sink.keys.Find("n");
    REQUIRE(nKey != INVALID_KEY_ID);
    CHECK(std::get<int64_t>(lines[0]->GetValue(nKey)) == 1);
    CHECK(std::get<int64_t>(lines[2]->GetValue(nKey)) == 3);

    // Raw line text is recoverable via the source.
    CHECK(source.RawLine(1) == R"({"k":"hello","n":1})");
    CHECK(source.RawLine(3) == R"({"k":"three","n":3})");
}

TEST_CASE(
    "JsonParser::ParseStreaming(StreamLineSource&) reports per-line errors with absolute line numbers",
    "[json_parser][stream_line_source]"
)
{
    using namespace loglib;

    const std::string payload = R"({"k":"ok"})"
                                "\n"
                                "not-json\n"
                                R"({"k":"second"})"
                                "\n";

    StreamLineSource source(std::filesystem::path("memory.log"), std::make_unique<InMemoryProducer>(payload));

    CollectingStreamSink sink;
    const JsonParser parser;
    parser.ParseStreaming(source, sink, ParserOptions{});

    REQUIRE(sink.finished);

    size_t goodLines = 0;
    std::vector<std::string> errors;
    for (auto &batch : sink.batches)
    {
        goodLines += batch.lines.size();
        errors.insert(errors.end(), batch.errors.begin(), batch.errors.end());
    }
    CHECK(goodLines == 2);
    REQUIRE(errors.size() == 1);
    INFO(errors.front());
    // Absolute line number 2 is the bad line; the surrounding "Error on
    // line N:" wrapper is composed by the streaming pipeline.
    CHECK(errors.front().contains("Error on line 2:"));
}
