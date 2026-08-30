#include <loglib/enum_dictionary.hpp>
#include <loglib/log_factory.hpp>
#include <loglib/log_table.hpp>
#include <loglib/time_zone_context.hpp>

#include <iostream>
#include <string_view>

int main()
{
    const loglib::TimeZoneContext utc;
    if (utc.IanaName() != std::string_view{"UTC"})
    {
        std::cerr << "TimeZoneContext default is not UTC\n";
        return 1;
    }

    loglib::LogTable table;
    if (table.RowCount() != 0)
    {
        std::cerr << "empty LogTable is not empty\n";
        return 2;
    }

    const auto parser = loglib::LogFactory::Create(loglib::LogFactory::Parser::Json);
    if (!parser)
    {
        std::cerr << "LogFactory::Create(Json) returned null\n";
        return 3;
    }

    loglib::EnumDictionary dict;
    (void)dict;
    return 0;
}
