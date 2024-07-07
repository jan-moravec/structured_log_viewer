#pragma once

#include "log_data.hpp"

#include <string>
#include <filesystem>

namespace loglib
{

struct ParseResult
{
    LogData data;
    std::string error;
};

class LogParser
{
public:
    virtual ~LogParser() = default;

    virtual ParseResult Parse(const std::filesystem::path &file) const = 0;
};

}