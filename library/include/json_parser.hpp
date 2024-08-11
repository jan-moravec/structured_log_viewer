#pragma once

#include "log_parser.hpp"

namespace loglib
{

class JsonParser : public LogParser
{
public:
    bool IsValid(const std::filesystem::path &file) const override;
    ParseResult Parse(const std::filesystem::path &file) const override;
};

} // namespace loglib
