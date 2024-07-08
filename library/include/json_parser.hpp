#pragma once

#include "log_parser.hpp"

namespace loglib
{

class JsonParser: public LogParser
{
public:
    ParseResult Parse(const std::filesystem::path &file) const override;
};

}
