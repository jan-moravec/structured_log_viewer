#include <catch2/catch_all.hpp>
#include <loglib/log_processing.hpp>

TEST_CASE("LogProcessing Initialize and ParseTimestamps", "[log_processing]")
{
#ifdef _WIN32
    const auto tzdata = std::filesystem::current_path() / std::filesystem::path("tzdata");
#else
    const char *appDir = std::getenv("APPDIR");
    const auto tzdata = appDir ? std::filesystem::path(appDir) / std::filesystem::path("usr/share/tzdata")
                               : std::filesystem::current_path() + std::filesystem::path("tzdata");
#endif
    loglib::Initialize(tzdata);

    loglib::LogData data({}, {});
    loglib::LogConfiguration config;
    config.columns.push_back({"timestamp", {"time"}, "%F %T", loglib::LogConfiguration::Type::Time, {"%F %T"}});

    std::string errors = loglib::ParseTimestamps(data, config);
    REQUIRE(errors.empty());
}
