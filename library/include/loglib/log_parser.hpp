#pragma once

#include "log_data.hpp"
#include "log_line.hpp"
#include "parser_options.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace loglib
{

class FileLineSource;
class LogParseSink;
class StreamLineSource;

/// Buffered output of a synchronous parse.
struct ParseResult
{
    LogData data;
    std::vector<std::string> errors;
};

/// Base class for log-format parsers. New formats implement `IsValid`,
/// both `ParseStreaming` overloads, and `ToString`. The synchronous
/// "parse a file" helper is `loglib::ParseFile` (see `parse_file.hpp`);
/// production GUI code uses `ParseStreaming` directly.
class LogParser
{
public:
    virtual ~LogParser() = default;

    virtual bool IsValid(const std::filesystem::path &file) const = 0;

    /// Static-file streaming entry. Emitted `LogLine`s carry @p source
    /// and the line's 0-based file id.
    virtual void ParseStreaming(FileLineSource &source, LogParseSink &sink, ParserOptions options = {}) const = 0;

    /// Live-tail streaming entry. Emitted `LogLine`s carry @p source
    /// and the 1-based monotonic id assigned by `AppendLine`.
    virtual void ParseStreaming(StreamLineSource &source, LogParseSink &sink, ParserOptions options = {}) const = 0;

    /// Renders a parsed line back to the parser's native text form.
    virtual std::string ToString(const LogLine &line) const = 0;
};

} // namespace loglib
