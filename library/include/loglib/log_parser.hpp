#pragma once

#include "log_data.hpp"

#include <filesystem>
#include <string>

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

    virtual bool IsValid(const std::filesystem::path &file) const = 0;
    virtual ParseResult Parse(const std::filesystem::path &file) const = 0;
};

} // namespace loglib
