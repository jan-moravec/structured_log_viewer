#include <catch2/catch_all.hpp>
#include <loglib/log_data.hpp>

TEST_CASE("LogData GetLines and GetKeys", "[log_data]")
{
    std::vector<std::unique_ptr<loglib::LogLine>> lines;
    std::vector<std::string> keys = {"key1", "key2"};

    loglib::LogData data(std::move(lines), keys);

    REQUIRE(data.GetLines().empty());
    REQUIRE(data.GetKeys() == keys);
}
