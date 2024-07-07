#pragma once

#include "log_parser.hpp"

//using UtcTimeStamp = std::chrono::time_point<std::chrono::system_clock, std::chrono::microseconds>;
//using LogValue = std::variant<std::string, int64_t, double, bool, std::monostate>;

namespace loglib
{

class JsonParser: public LogParser
{
public:
    ParseResult Parse(const std::filesystem::path &file) const override;
};

}
