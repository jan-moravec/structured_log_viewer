#include "loglib/core/row_batch.hpp"

#include <fmt/format.h>

namespace loglib
{

std::string FormatParseError(const ParseError &error)
{
    if (error.firstLine == 0)
    {
        return error.message;
    }
    if (error.lastLine > error.firstLine)
    {
        return fmt::format("Error on lines {}-{}: {}", error.firstLine, error.lastLine, error.message);
    }
    return fmt::format("Error on line {}: {}", error.firstLine, error.message);
}

} // namespace loglib
