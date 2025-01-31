#include <catch2/catch_all.hpp>
#include <loglib/json_line.hpp>

TEST_CASE("JsonLine GetRawValue and GetLine", "[json_line]")
{
    nlohmann::json json = {{"key", "value"}};
    loglib::JsonLine line(std::move(json));

    REQUIRE(std::get<std::string>(line.GetRawValue("key")) == "value");
    REQUIRE(line.GetLine() == "{\"key\":\"value\"}");
}
