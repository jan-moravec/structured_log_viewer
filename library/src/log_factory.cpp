#include "loglib/log_factory.hpp"

#include "loglib/parsers/json_parser.hpp"

#include <stdexcept>

namespace loglib
{

std::unique_ptr<LogParser> LogFactory::Create(Parser parser)
{
    switch (parser)
    {
    case Parser::Json:
        return std::make_unique<JsonParser>();
    default:
        throw std::runtime_error("Invalid parser " + std::to_string(static_cast<int>(parser)));
    }
}

} // namespace loglib
