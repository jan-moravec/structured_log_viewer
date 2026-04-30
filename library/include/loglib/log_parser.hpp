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
class LogSource;
class StreamingLogSink;

/// Buffered output of a synchronous parse.
struct ParseResult
{
    LogData data;
    std::vector<std::string> errors;
};

/// Base class for log-format parsers. New formats implement `IsValid`,
/// `ParseStreaming(LogSource&, ...)`, and `ToString`; `Parse(path)` and
/// the `LogFile&` overload of `ParseStreaming` are non-virtual conveniences
/// implemented in terms of the source-based virtual.
class LogParser
{
public:
    virtual ~LogParser() = default;

    virtual bool IsValid(const std::filesystem::path &file) const = 0;

    /// Synchronous parse: routes through `ParseStreaming` into a `BufferingSink`.
    ParseResult Parse(const std::filesystem::path &file) const;

    /// Streams parsed lines from @p source into @p sink. `options.stopToken`
    /// is polled for cooperative cancellation; when `options.configuration`
    /// is non-null the pipeline promotes `Type::time` columns inline.
    /// Concrete parsers implement this; the `LogFile&` overload below is a
    /// non-virtual wrapper that constructs a borrowing `MappedFileSource`
    /// and forwards (PRD 4.9.3).
    virtual void ParseStreaming(LogSource &source, StreamingLogSink &sink, ParserOptions options = {}) const = 0;

    /// Backward-compatible overload for the static-file path: wraps @p file
    /// in a borrowing `MappedFileSource` and forwards to the source-based
    /// virtual. The caller retains ownership of @p file; the wrapper is
    /// stack-local.
    void ParseStreaming(LogFile &file, StreamingLogSink &sink, ParserOptions options = {}) const;

    /// Renders a parsed line back to the parser's native text form.
    virtual std::string ToString(const LogLine &line) const = 0;
};

} // namespace loglib
