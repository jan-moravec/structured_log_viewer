
#include "common.hpp"

#include <loglib/log_factory.hpp>
#include <loglib/parse_file.hpp>

#include <catch2/catch_all.hpp>
#include <glaze/glaze.hpp>

#include <fstream>
#include <string_view>

using namespace loglib;

TEST_CASE("Create JSON logs parser", "[log_factory]")
{
    auto parser = LogFactory::Create(LogFactory::Parser::Json);
    CHECK(parser != nullptr);
}

TEST_CASE("Create logfmt parser", "[log_factory]")
{
    auto parser = LogFactory::Create(LogFactory::Parser::Logfmt);
    CHECK(parser != nullptr);
}

TEST_CASE("Create non-existent parser", "[log_factory]")
{
    CHECK_THROWS_AS(LogFactory::Create(LogFactory::Parser::Count), std::runtime_error);
}

TEST_CASE("Parse JSON log file via ParseFile auto-detect", "[log_factory]")
{
    const TestJsonLogFile testFile(glz::generic_sorted_u64{{"key", "value"}});

    ParseResult result = ParseFile(testFile.GetFilePath());
    CHECK(result.errors.empty());
    CHECK(result.data.Lines().size() == 1);
}

TEST_CASE("Parse logfmt log file via ParseFile auto-detect", "[log_factory]")
{
    // `TestJsonLogFile` is reused only for path/cleanup; the body
    // is logfmt bytes.
    const TestJsonLogFile holder(std::string{"autodetect.logfmt"});
    {
        std::ofstream file(holder.GetFilePath(), std::ios::trunc);
        REQUIRE(file.is_open());
        file << "level=info msg=\"hello\"\n";
    }

    ParseResult result = ParseFile(holder.GetFilePath());
    CHECK(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 1);
    CHECK(AsStringView(result.data.Lines()[0].GetValue("msg")) == std::string_view{"hello"});
}

TEST_CASE("ParseFile auto-detect rejects nonexistent or invalid file", "[log_factory]")
{
    CHECK_THROWS_AS(ParseFile("nonexistent.json"), std::runtime_error);

    // "Invalid log line." has no '=', so neither the JSON probe
    // nor the logfmt probe accepts it.
    const TestJsonLogFile testFile(TestJsonLogFile::Line("Invalid log line."));
    CHECK_THROWS_AS(ParseFile(testFile.GetFilePath()), std::runtime_error);
}
