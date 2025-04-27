#include "loglib/log_factory.hpp"

#include "loglib/json_parser.hpp"

#include <stdexcept>

namespace loglib
{

ParseResult LogFactory::Parse(const std::filesystem::path &file)
{
    for (int i = 0; i < static_cast<int>(Parser::Count); ++i)
    {
        Parser parserType = static_cast<Parser>(i);
        const std::unique_ptr<LogParser> parser = Create(parserType);
        if (parser->IsValid(file))
        {
            return parser->Parse(file);
        }
    }

    throw std::runtime_error("Input file '" + file.string() + "' could not be parsed.");
}

std::unique_ptr<LogParser> LogFactory::Create(Parser parser)
{
    switch (parser)
    {
    case Parser::Json:
        return std::make_unique<JsonParser>();
    default:
        throw std::runtime_error("Ivalid parser " + std::to_string(static_cast<int>(parser)));
    }
}

} // namespace loglib
