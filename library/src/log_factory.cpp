#include "loglib/log_factory.hpp"

#include "loglib/parsers/csv_parser.hpp"
#include "loglib/parsers/json_parser.hpp"
#include "loglib/parsers/logfmt_parser.hpp"
#include "loglib/parsers/regex_parser.hpp"

#include <stdexcept>

namespace loglib
{

std::unique_ptr<LogParser> LogFactory::Create(Parser parser)
{
    switch (parser)
    {
    case Parser::Json:
        return std::make_unique<JsonParser>();
    case Parser::Logfmt:
        return std::make_unique<LogfmtParser>();
    case Parser::Csv:
        return std::make_unique<CsvParser>();
    case Parser::Regex:
        // Default-constructed: no pinned pattern. Fine for
        // `IsValid` probes; `ParseStreaming` needs a pattern
        // supplied via `ParserOptions::configuration->source->
        // regexPattern` or an explicit-pattern overload.
        return std::make_unique<RegexParser>();
    default:
        throw std::runtime_error("Invalid parser " + std::to_string(static_cast<int>(parser)));
    }
}

} // namespace loglib
