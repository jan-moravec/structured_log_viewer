#include "common.hpp"

#include <loglib/json_parser.hpp>
#include <loglib/log_line.hpp>

#include <catch2/catch_all.hpp>
#include <nlohmann/json.hpp>

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
    TestJsonLogFile testFile; // Creates an empty file
    CHECK_FALSE(parser.IsValid(TestJsonLogFile::GetFilePath()));
}

TEST_CASE("Validate file with JSON line", "[json_parser]")
{
    loglib::JsonParser parser;
    TestJsonLogFile testFile(nlohmann::json{{"key", "value"}});
    CHECK(parser.IsValid(TestJsonLogFile::GetFilePath()));
}

TEST_CASE("Validate file with invalid line", "[json_parser]")
{
    loglib::JsonParser parser;
    TestJsonLogFile testFile(std::string("invalid json"));
    CHECK_FALSE(parser.IsValid(TestJsonLogFile::GetFilePath()));
}

TEST_CASE("Try parsing non-existent file", "[json_parser]")
{
    loglib::JsonParser parser;
    CHECK_THROWS_AS(parser.Parse("non_existent_file.json"), std::runtime_error);
}

TEST_CASE("Parse empty file", "[json_parser]")
{
    loglib::JsonParser parser;
    TestJsonLogFile testFile;

    auto result = parser.Parse(TestJsonLogFile::GetFilePath());
    CHECK(result.errors.empty());
    CHECK(result.data.Lines().empty());
    CHECK(result.data.Keys().empty());
    REQUIRE(result.data.Files().size() == 1);
    CHECK(result.data.Files()[0]->GetPath() == TestJsonLogFile::GetFilePath());
    CHECK_THROWS_AS(result.data.Files()[0]->GetLine(0), std::out_of_range);
}

TEST_CASE("Parse file with empty JSON object", "[json_parser]")
{
    loglib::JsonParser parser;
    TestJsonLogFile testFile(nlohmann::json::object());

    auto result = parser.Parse(TestJsonLogFile::GetFilePath());
    CHECK(result.errors.empty());
    REQUIRE(result.data.Lines().size() == testFile.JsonLines().size());
    CHECK(result.data.Lines()[0].Values().empty());
    CHECK(result.data.Keys().empty());
    REQUIRE(result.data.Files().size() == 1);
    CHECK(result.data.Files()[0]->GetPath() == TestJsonLogFile::GetFilePath());
    CHECK(result.data.Files()[0]->GetLine(0) == "{}");
}

TEST_CASE("Parse file with multiple empty JSON objects", "[json_parser]")
{
    loglib::JsonParser parser;
    TestJsonLogFile testFile({nlohmann::json::object(), nlohmann::json::object()});

    auto result = parser.Parse(TestJsonLogFile::GetFilePath());
    CHECK(result.errors.empty());
    REQUIRE(result.data.Lines().size() == testFile.JsonLines().size());
    CHECK(result.data.Keys().empty());
    REQUIRE(result.data.Files().size() == 1);
    CHECK(result.data.Files()[0]->GetPath() == TestJsonLogFile::GetFilePath());
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
        TestJsonLogFile testFile(nlohmann::json{{"key", nullptr}});
        auto result = parser.Parse(TestJsonLogFile::GetFilePath());
        CHECK(result.errors.empty());
        REQUIRE(result.data.Lines().size() == testFile.JsonLines().size());
        CHECK(std::holds_alternative<std::monostate>(result.data.Lines()[0].GetValue("key")));
        REQUIRE(result.data.Keys().size() == 1);
        CHECK(result.data.Keys()[0] == "key");
        REQUIRE(result.data.Files().size() == 1);
        CHECK(result.data.Files()[0]->GetPath() == TestJsonLogFile::GetFilePath());
        CHECK(result.data.Files()[0]->GetLine(0) == R"({"key":null})");
    }

    SECTION("String")
    {
        TestJsonLogFile testFile(nlohmann::json{{"key", "value"}});
        auto result = parser.Parse(TestJsonLogFile::GetFilePath());
        CHECK(result.errors.empty());
        REQUIRE(result.data.Lines().size() == testFile.JsonLines().size());
        CHECK(std::get<std::string>(result.data.Lines()[0].GetValue("key")) == "value");
        REQUIRE(result.data.Keys().size() == 1);
        CHECK(result.data.Keys()[0] == "key");
        REQUIRE(result.data.Files().size() == 1);
        CHECK(result.data.Files()[0]->GetPath() == TestJsonLogFile::GetFilePath());
        CHECK(result.data.Files()[0]->GetLine(0) == R"({"key":"value"})");
    }

    SECTION("Unsigned integer")
    {
        TestJsonLogFile testFile(nlohmann::json{{"key", 42}});
        auto result = parser.Parse(TestJsonLogFile::GetFilePath());
        CHECK(result.errors.empty());
        REQUIRE(result.data.Lines().size() == testFile.JsonLines().size());
        CHECK(std::get<uint64_t>(result.data.Lines()[0].GetValue("key")) == 42);
        REQUIRE(result.data.Keys().size() == 1);
        CHECK(result.data.Keys()[0] == "key");
        REQUIRE(result.data.Files().size() == 1);
        CHECK(result.data.Files()[0]->GetPath() == TestJsonLogFile::GetFilePath());
        CHECK(result.data.Files()[0]->GetLine(0) == R"({"key":42})");
    }

    SECTION("Integer")
    {
        TestJsonLogFile testFile(nlohmann::json{{"key", -12}});
        auto result = parser.Parse(TestJsonLogFile::GetFilePath());
        CHECK(result.errors.empty());
        REQUIRE(result.data.Lines().size() == testFile.JsonLines().size());
        CHECK(std::get<int64_t>(result.data.Lines()[0].GetValue("key")) == -12);
        REQUIRE(result.data.Keys().size() == 1);
        CHECK(result.data.Keys()[0] == "key");
        REQUIRE(result.data.Files().size() == 1);
        CHECK(result.data.Files()[0]->GetPath() == TestJsonLogFile::GetFilePath());
        CHECK(result.data.Files()[0]->GetLine(0) == R"({"key":-12})");
    }

    SECTION("Double")
    {
        TestJsonLogFile testFile(nlohmann::json{{"key", 3.14}});
        auto result = parser.Parse(TestJsonLogFile::GetFilePath());
        CHECK(result.errors.empty());
        REQUIRE(result.data.Lines().size() == testFile.JsonLines().size());
        CHECK(std::get<double>(result.data.Lines()[0].GetValue("key")) == Catch::Approx(3.14));
        REQUIRE(result.data.Keys().size() == 1);
        CHECK(result.data.Keys()[0] == "key");
        REQUIRE(result.data.Files().size() == 1);
        CHECK(result.data.Files()[0]->GetPath() == TestJsonLogFile::GetFilePath());
        CHECK(result.data.Files()[0]->GetLine(0) == R"({"key":3.14})");
    }

    SECTION("Boolean")
    {
        TestJsonLogFile testFile(nlohmann::json{{"key", true}});
        auto result = parser.Parse(TestJsonLogFile::GetFilePath());
        REQUIRE(result.errors.empty());
        CHECK(result.data.Lines().size() == testFile.JsonLines().size());
        CHECK(std::get<bool>(result.data.Lines()[0].GetValue("key")) == true);
        REQUIRE(result.data.Keys().size() == 1);
        CHECK(result.data.Keys()[0] == "key");
        REQUIRE(result.data.Files().size() == 1);
        CHECK(result.data.Files()[0]->GetPath() == TestJsonLogFile::GetFilePath());
        CHECK(result.data.Files()[0]->GetLine(0) == R"({"key":true})");
    }
}

TEST_CASE("Parse file with single JSON object containing all possible JSON elements", "[json_parser]")
{
    loglib::JsonParser parser;
    TestJsonLogFile testFile(nlohmann::json{
        {"null", nullptr}, {"string", "value"}, {"uinteger", 42}, {"integer", -12}, {"double", 3.14}, {"boolean", true}
    });

    auto result = parser.Parse(TestJsonLogFile::GetFilePath());
    CHECK(result.errors.empty());
    REQUIRE(result.data.Lines().size() == testFile.JsonLines().size());

    const auto &values = result.data.Lines()[0].Values();
    CHECK(std::holds_alternative<std::monostate>(values.at("null")));
    CHECK(std::get<std::string>(values.at("string")) == "value");
    CHECK(std::get<uint64_t>(values.at("uinteger")) == 42);
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
    CHECK(result.data.Files()[0]->GetPath() == TestJsonLogFile::GetFilePath());
    CHECK(
        result.data.Files()[0]->GetLine(0) ==
        R"({"boolean":true,"double":3.14,"integer":-12,"null":null,"string":"value","uinteger":42})"
    );
}

TEST_CASE("Parse file with multiple JSON objects", "[json_parser]")
{
    loglib::JsonParser parser;
    TestJsonLogFile testFile({nlohmann::json{{"key1", "value1"}}, nlohmann::json{{"key2", "value2"}}});

    auto result = parser.Parse(TestJsonLogFile::GetFilePath());
    CHECK(result.errors.empty());
    REQUIRE(result.data.Lines().size() == testFile.JsonLines().size());
    CHECK(std::get<std::string>(result.data.Lines()[0].GetValue("key1")) == "value1");
    CHECK(std::get<std::string>(result.data.Lines()[1].GetValue("key2")) == "value2");
    REQUIRE(result.data.Keys().size() == 2);
    CHECK(result.data.Keys()[0] == "key1");
    CHECK(result.data.Keys()[1] == "key2");
    REQUIRE(result.data.Files().size() == 1);
    CHECK(result.data.Files()[0]->GetPath() == TestJsonLogFile::GetFilePath());
    CHECK(result.data.Files()[0]->GetLine(0) == R"({"key1":"value1"})");
    CHECK(result.data.Files()[0]->GetLine(1) == R"({"key2":"value2"})");
}

TEST_CASE("Parse file with multiple JSON objects and one invalid line", "[json_parser]")
{
    loglib::JsonParser parser;
    TestJsonLogFile testFile(
        {nlohmann::json{{"key1", "value1"}}, std::string("invalid json"), nlohmann::json{{"key2", "value2"}}}
    );

    auto result = parser.Parse(TestJsonLogFile::GetFilePath());
    CHECK(result.errors.size() == testFile.StringLines().size());
    REQUIRE(result.data.Lines().size() == testFile.JsonLines().size());
    CHECK(std::get<std::string>(result.data.Lines()[0].GetValue("key1")) == "value1");
    CHECK(std::get<std::string>(result.data.Lines()[1].GetValue("key2")) == "value2");
    REQUIRE(result.data.Keys().size() == 2);
    CHECK(result.data.Keys()[0] == "key1");
    CHECK(result.data.Keys()[1] == "key2");
    REQUIRE(result.data.Files().size() == 1);
    CHECK(result.data.Files()[0]->GetPath() == TestJsonLogFile::GetFilePath());
    CHECK(result.data.Files()[0]->GetLine(0) == R"({"key1":"value1"})");
    CHECK(result.data.Files()[0]->GetLine(1) == "invalid json");
    CHECK(result.data.Files()[0]->GetLine(2) == R"({"key2":"value2"})");
}

TEST_CASE("Parse file with one invalid line", "[json_parser]")
{
    loglib::JsonParser parser;
    TestJsonLogFile testFile(std::string("invalid json"));

    auto result = parser.Parse(TestJsonLogFile::GetFilePath());
    CHECK(result.data.Lines().empty());
    CHECK(result.errors.size() == testFile.StringLines().size());
}

TEST_CASE("Parse file with one multiple invalid lines", "[json_parser]")
{
    loglib::JsonParser parser;
    TestJsonLogFile testFile({std::string("invalid json 1"), std::string("invalid json 2")});

    auto result = parser.Parse(TestJsonLogFile::GetFilePath());
    CHECK(result.data.Lines().empty());
    CHECK(result.errors.size() == testFile.StringLines().size());
}

TEST_CASE("Parse file with multiple JSON objects and multiple invalid lines", "[json_parser]")
{
    loglib::JsonParser parser;
    TestJsonLogFile testFile(
        {nlohmann::json{{"key1", "value1"}},
         std::string("invalid json 1"),
         nlohmann::json{{"key2", "value2"}},
         std::string("invalid json 2")}
    );

    auto result = parser.Parse(TestJsonLogFile::GetFilePath());
    CHECK(result.errors.size() == testFile.StringLines().size());
    REQUIRE(result.data.Lines().size() == testFile.JsonLines().size());
    CHECK(std::get<std::string>(result.data.Lines()[0].GetValue("key1")) == "value1");
    CHECK(std::get<std::string>(result.data.Lines()[1].GetValue("key2")) == "value2");
    REQUIRE(result.data.Files().size() == 1);
    CHECK(result.data.Files()[0]->GetPath() == TestJsonLogFile::GetFilePath());
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
