#pragma once

#include "log_data.hpp"
#include "log_line.hpp"
#include "parser_options.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace loglib
{

class LogFile;
class StreamingLogSink;

/// Buffered output of a synchronous parse.
struct ParseResult
{
    LogData data;
    std::vector<std::string> errors;
};

/// Base class for log-format parsers. New formats implement `IsValid`,
/// `ParseStreaming`, and `ToString`; `Parse(path)` is a non-virtual
/// convenience that buffers the streaming output.
class LogParser
{
public:
    virtual ~LogParser() = default;

    virtual bool IsValid(const std::filesystem::path &file) const = 0;

    /// Synchronous parse: routes through `ParseStreaming` into a `BufferingSink`.
    ParseResult Parse(const std::filesystem::path &file) const;

    /// Streams parsed lines into @p sink. `options.stopToken` is polled for
    /// cooperative cancellation; when `options.configuration` is non-null the
    /// pipeline promotes `Type::time` columns inline.
    virtual void ParseStreaming(LogFile &file, StreamingLogSink &sink, ParserOptions options = {}) const = 0;

    /// Renders a parsed line back to the parser's native text form.
    virtual std::string ToString(const LogLine &line) const = 0;
};

} // namespace loglib
