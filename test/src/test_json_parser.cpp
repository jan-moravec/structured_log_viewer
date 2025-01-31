#include <catch2/catch_all.hpp>
#include <loglib/json_parser.hpp>

TEST_CASE("JsonParser IsValid and Parse", "[json_parser]")
{
    loglib::JsonParser parser;
    REQUIRE(parser.IsValid("test.json") == false);

    auto result = parser.Parse("test.json");
    REQUIRE(result.error.empty());
    REQUIRE(result.data.GetLines().empty());
}
