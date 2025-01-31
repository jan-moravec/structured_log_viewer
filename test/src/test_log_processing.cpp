#include <catch2/catch_all.hpp>
#include <loglib/log_processing.hpp>

TEST_CASE("LogProcessing Initialize and ParseTimestamps", "[log_processing]")
{
    loglib::Initialize();

    loglib::LogData data({}, {});
    loglib::LogConfiguration config;
    config.columns.push_back({"timestamp", {"time"}, "%F %T", loglib::LogConfiguration::Type::Time, {"%F %T"}});

    std::string errors = loglib::ParseTimestamps(data, config);
    REQUIRE(errors.empty());
}
