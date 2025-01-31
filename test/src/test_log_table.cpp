#include <catch2/catch_all.hpp>
#include <loglib/log_table.hpp>

TEST_CASE("LogTable GetHeader and ColumnCount", "[log_table]")
{
    loglib::LogConfiguration config;
    config.columns.push_back({"header1", {"key1"}, "{}"});
    config.columns.push_back({"header2", {"key2"}, "{}"});

    loglib::LogData data({}, {});
    loglib::LogTable table(data, config);

    REQUIRE(table.GetHeader(0) == "header1");
    REQUIRE(table.GetHeader(1) == "header2");
    REQUIRE(table.ColumnCount() == 2);
}
