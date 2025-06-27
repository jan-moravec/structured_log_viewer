
#include "common.hpp"

#include <loglib/log_factory.hpp>

#include <catch2/catch_all.hpp>
#include <glaze/glaze.hpp>

using namespace loglib;

TEST_CASE("Create JSON logs parser", "[log_factory]")
{
    LogFactory factory;
    auto parser = factory.Create(LogFactory::Parser::Json);
    CHECK(parser != nullptr);
}

TEST_CASE("Create non-existent parser", "[log_factory]")
{
    LogFactory factory;
    CHECK_THROWS_AS(factory.Create(LogFactory::Parser::Count), std::runtime_error);
}

TEST_CASE("Parse JSON log file", "[log_factory]")
{
    TestJsonLogFile testFile(glz::json_t{{"key", "value"}});

    LogFactory factory;
    ParseResult result = factory.Parse(testFile.GetFilePath());
    CHECK(result.errors.empty());
    CHECK(result.data.Lines().size() == 1);
}

TEST_CASE("Parse nonexistent or invalid file", "[log_factory]")
{
    LogFactory factory;
    CHECK_THROWS_AS(factory.Parse("nonexistent.json"), std::runtime_error);

    TestJsonLogFile testFile(TestJsonLogFile::Line("Invalid log line."));
    CHECK_THROWS_AS(factory.Parse(testFile.GetFilePath()), std::runtime_error);
}
