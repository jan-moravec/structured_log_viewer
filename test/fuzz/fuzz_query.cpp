// Fuzz target for the filter query parser. Every query that parses is written back with FormatExpression and parsed
// again. The second parse must succeed and give an equal expression, because that round trip is the documented
// contract of FormatExpression.

#include "fuzz_common.hpp"

#include "loglib/query/query_parser.hpp"
#include "loglib/time/time_zone_context.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size)
{
    static const loglib::TimeZoneContext ZONE = loglib::TimeZoneContext::Utc();

    const std::string_view input = loglib::fuzz::AsText(data, size);
    const auto parsed = loglib::ParseQuery(input, ZONE);
    if (!parsed)
    {
        return 0;
    }

    const std::string printed = loglib::FormatExpression(*parsed);
    const auto reparsed = loglib::ParseQuery(printed, ZONE);
    if (!reparsed)
    {
        std::cerr << "printed query does not parse: " << printed << '\n';
        std::abort();
    }
    if (!(*reparsed == *parsed))
    {
        std::cerr << "printed query parses to a different expression: " << printed << '\n';
        std::abort();
    }
    return 0;
}
