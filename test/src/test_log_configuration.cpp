#include <catch2/catch_all.hpp>
#include <loglib/log_configuration.hpp>

TEST_CASE("LogConfiguration Update and Serialize", "[log_configuration]")
{
    loglib::LogConfiguration config;
    loglib::LogData data({}, {"key1", "key2"});

    loglib::UpdateConfiguration(config, data);
    REQUIRE(config.columns.size() == 2);

    loglib::SerializeConfiguration("config.json", config);
    auto deserializedConfig = loglib::DeserializeConfiguration("config.json");
    REQUIRE(deserializedConfig.columns.size() == 2);
}
