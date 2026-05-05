#pragma once

#include "log_parser.hpp"

#include <memory>

namespace loglib
{

class LogFactory
{
public:
    enum class Parser
    {
        Json,
        Count
    };

    static std::unique_ptr<LogParser> Create(Parser parser);
};

} // namespace loglib
