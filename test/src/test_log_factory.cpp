#include <catch2/catch_all.hpp>
#include <loglib/log_factory.hpp>

TEST_CASE("LogFactory Parse and Create", "[log_factory]")
{
    loglib::LogFactory factory;
    auto result = factory.Parse("test.json");

    REQUIRE(result.error.empty());
    REQUIRE(result.data.GetLines().empty());
}
