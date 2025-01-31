#pragma once

#include "log_parser.hpp"

#include <filesystem>
#include <memory>

namespace loglib
{

class LogFactory
{
public:
    static ParseResult Parse(const std::filesystem::path &file);

    enum class Parser
    {
        Json,
        Count
    };

    static std::unique_ptr<LogParser> Create(Parser parser);
};

} // namespace loglib
