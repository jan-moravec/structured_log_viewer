#include "common.hpp"

#include <loglib/json_parser.hpp>
#include <loglib/log_line.hpp>

#include <catch2/catch_all.hpp>
#include <glaze/glaze.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <random>

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
    loglib::JsonParser parser;
    TestJsonLogFile testFile(TestJsonLogFile::Line("\n\n\n"));
    CHECK_THROWS_AS(parser.Parse(testFile.GetFilePath()), std::runtime_error);
}

TEST_CASE("Parse file with invalid line", "[json_parser]")
{
    loglib::JsonParser parser;
    TestJsonLogFile testFile(TestJsonLogFile::Line("invalid json"));
    CHECK_THROWS_AS(parser.Parse(testFile.GetFilePath()), std::runtime_error);
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
    CHECK_THROWS_AS(parser.Parse(testFile.GetFilePath()), std::runtime_error);
}

TEST_CASE("Parse file with empty JSON object", "[json_parser]")
{
    loglib::JsonParser parser;
    TestJsonLogFile testFile(TestJsonLogFile::Line(R"({})"));

    auto result = parser.Parse(testFile.GetFilePath());
    CHECK(result.errors.empty());
    REQUIRE(result.data.Lines().size() == testFile.JsonLines().size());
    CHECK(result.data.Lines()[0].Values().empty());
    CHECK(result.data.Keys().empty());
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
    CHECK(result.data.Keys().empty());
    REQUIRE(result.data.Files().size() == 1);
    CHECK(result.data.Files()[0]->GetPath() == testFile.GetFilePath());
    for (size_t i = 0; i < result.data.Lines().size(); i++)
    {
        CHECK(result.data.Lines()[i].Values().empty());
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
        REQUIRE(result.data.Keys().size() == 1);
        CHECK(result.data.Keys()[0] == "key");
        REQUIRE(result.data.Files().size() == 1);
        CHECK(result.data.Files()[0]->GetPath() == testFile.GetFilePath());
        CHECK(result.data.Files()[0]->GetLine(0) == R"({"key":null})");
    }

    SECTION("String")
    {
        TestJsonLogFile testFile(TestJsonLogFile::Line(R"({"key": "value"})"));
        auto result = parser.Parse(testFile.GetFilePath());
        CHECK(result.errors.empty());
        REQUIRE(result.data.Lines().size() == testFile.JsonLines().size());
        CHECK(std::get<std::string>(result.data.Lines()[0].GetValue("key")) == "value");
        REQUIRE(result.data.Keys().size() == 1);
        CHECK(result.data.Keys()[0] == "key");
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
        CHECK(std::get<uint64_t>(result.data.Lines()[0].GetValue("key")) == 10000000000000000000);
        REQUIRE(result.data.Keys().size() == 1);
        CHECK(result.data.Keys()[0] == "key");
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
        REQUIRE(result.data.Keys().size() == 1);
        CHECK(result.data.Keys()[0] == "key");
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
        REQUIRE(result.data.Keys().size() == 1);
        CHECK(result.data.Keys()[0] == "key");
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
        REQUIRE(result.data.Keys().size() == 1);
        CHECK(result.data.Keys()[0] == "key");
        REQUIRE(result.data.Files().size() == 1);
        CHECK(result.data.Files()[0]->GetPath() == testFile.GetFilePath());
        CHECK(result.data.Files()[0]->GetLine(0) == R"({"key":true})");
    }
}

TEST_CASE("Parse file with single JSON object containing all possible JSON elements", "[json_parser]")
{
    loglib::JsonParser parser;
    TestJsonLogFile testFile(glz::json_t{
        {"null", nullptr},
        {"string", "value"},
        {"uinteger", 10000000000000000000},
        {"integer", -12},
        {"double", 3.14},
        {"boolean", true}
    });

    auto result = parser.Parse(testFile.GetFilePath());
    CHECK(result.errors.empty());
    REQUIRE(result.data.Lines().size() == testFile.JsonLines().size());

    const auto &values = result.data.Lines()[0].Values();
    CHECK(std::holds_alternative<std::monostate>(values.at("null")));
    CHECK(std::get<std::string>(values.at("string")) == "value");
    CHECK(std::get<uint64_t>(values.at("uinteger")) == 10000000000000000000);
    CHECK(std::get<int64_t>(values.at("integer")) == -12);
    CHECK(std::get<double>(values.at("double")) == Catch::Approx(3.14));
    CHECK(std::get<bool>(values.at("boolean")) == true);
    REQUIRE(result.data.Keys().size() == 6);
    CHECK(result.data.Keys()[0] == "boolean");
    CHECK(result.data.Keys()[1] == "double");
    CHECK(result.data.Keys()[2] == "integer");
    CHECK(result.data.Keys()[3] == "null");
    CHECK(result.data.Keys()[4] == "string");
    CHECK(result.data.Keys()[5] == "uinteger");
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
        {glz::json_t{{"1", nullptr}, {"2", "value"}, {"3", 10000000000000000000}, {"4", -12}, {"5", 3.14}, {"6", true}},
         glz::json_t{{"6", nullptr}, {"1", "value"}, {"2", 10000000000000000000}, {"3", -12}, {"4", 3.14}, {"5", true}},
         glz::json_t{{"5", nullptr}, {"6", "value"}, {"1", 10000000000000000000}, {"2", -12}, {"3", 3.14}, {"4", true}}}
    );

    auto result = parser.Parse(testFile.GetFilePath());
    CHECK(result.errors.empty());
    REQUIRE(result.data.Lines().size() == testFile.JsonLines().size());

    const auto &values = result.data.Lines()[0].Values();
    CHECK(std::holds_alternative<std::monostate>(values.at("1")));
    CHECK(std::get<std::string>(values.at("2")) == "value");
    CHECK(std::get<uint64_t>(values.at("3")) == 10000000000000000000);
    CHECK(std::get<int64_t>(values.at("4")) == -12);
    CHECK(std::get<double>(values.at("5")) == Catch::Approx(3.14));
    CHECK(std::get<bool>(values.at("6")) == true);

    const auto &values1 = result.data.Lines()[1].Values();
    CHECK(std::holds_alternative<std::monostate>(values1.at("6")));
    CHECK(std::get<std::string>(values1.at("1")) == "value");
    CHECK(std::get<uint64_t>(values1.at("2")) == 10000000000000000000);
    CHECK(std::get<int64_t>(values1.at("3")) == -12);
    CHECK(std::get<double>(values1.at("4")) == Catch::Approx(3.14));
    CHECK(std::get<bool>(values1.at("5")) == true);

    const auto &values2 = result.data.Lines()[2].Values();
    CHECK(std::holds_alternative<std::monostate>(values2.at("5")));
    CHECK(std::get<std::string>(values2.at("6")) == "value");
    CHECK(std::get<uint64_t>(values2.at("1")) == 10000000000000000000);
    CHECK(std::get<int64_t>(values2.at("2")) == -12);
    CHECK(std::get<double>(values2.at("3")) == Catch::Approx(3.14));
    CHECK(std::get<bool>(values2.at("4")) == true);

    REQUIRE(result.data.Keys().size() == 6);
    CHECK(result.data.Keys()[0] == "1");
    CHECK(result.data.Keys()[1] == "2");
    CHECK(result.data.Keys()[2] == "3");
    CHECK(result.data.Keys()[3] == "4");
    CHECK(result.data.Keys()[4] == "5");
    CHECK(result.data.Keys()[5] == "6");

    REQUIRE(result.data.Files().size() == 1);
    CHECK(result.data.Files()[0]->GetPath() == testFile.GetFilePath());
}

TEST_CASE("Parse file with multiple JSON objects", "[json_parser]")
{
    loglib::JsonParser parser;
    TestJsonLogFile testFile({glz::json_t{{"key1", "value1"}}, glz::json_t{{"key2", "value2"}}});

    auto result = parser.Parse(testFile.GetFilePath());
    CHECK(result.errors.empty());
    REQUIRE(result.data.Lines().size() == testFile.JsonLines().size());
    CHECK(std::get<std::string>(result.data.Lines()[0].GetValue("key1")) == "value1");
    CHECK(std::get<std::string>(result.data.Lines()[1].GetValue("key2")) == "value2");
    REQUIRE(result.data.Keys().size() == 2);
    CHECK(result.data.Keys()[0] == "key1");
    CHECK(result.data.Keys()[1] == "key2");
    REQUIRE(result.data.Files().size() == 1);
    CHECK(result.data.Files()[0]->GetPath() == testFile.GetFilePath());
    CHECK(result.data.Files()[0]->GetLine(0) == R"({"key1":"value1"})");
    CHECK(result.data.Files()[0]->GetLine(1) == R"({"key2":"value2"})");
}

TEST_CASE("Parse file with multiple JSON objects and one invalid line", "[json_parser]")
{
    // Parse will end with first invalid line
    loglib::JsonParser parser;
    TestJsonLogFile testFile({glz::json_t{{"key1", "value1"}}, "invalid json", glz::json_t{{"key2", "value2"}}});

    auto result = parser.Parse(testFile.GetFilePath());
    CHECK(result.errors.size() == 1);
    REQUIRE(result.data.Lines().size() == 2);
    CHECK(std::get<std::string>(result.data.Lines()[0].GetValue("key1")) == "value1");
    CHECK(std::get<std::string>(result.data.Lines()[1].GetValue("key2")) == "value2");
    REQUIRE(result.data.Keys().size() == 2);
    CHECK(result.data.Keys()[0] == "key1");
    CHECK(result.data.Keys()[1] == "key2");
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
        {glz::json_t{{"key1", "value1"}}, "invalid json 1", glz::json_t{{"key2", "value2"}}, "invalid json 2"}
    );

    auto result = parser.Parse(testFile.GetFilePath());
    CHECK(result.errors.size() == 2);
    REQUIRE(result.data.Keys().size() == 2);
    CHECK(std::get<std::string>(result.data.Lines()[0].GetValue("key1")) == "value1");
    CHECK(std::get<std::string>(result.data.Lines()[1].GetValue("key2")) == "value2");
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
