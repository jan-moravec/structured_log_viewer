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
/// the two `ParseStreaming(LineSource&, ...)` virtuals, and `ToString`.
///
/// The synchronous "parse a file to a `ParseResult`" helper used to
/// live here as `Parse(path)`; it is now the free function
/// `loglib::ParseFile(parser, path)` declared in `loglib/parse_file.hpp`.
/// Production GUI code never goes through that helper -- the static-file
/// open path runs through the streaming entry below.
class LogParser
{
public:
    virtual ~LogParser() = default;

    virtual bool IsValid(const std::filesystem::path &file) const = 0;

    /// Static-file streaming entry point: emits `LogLine`s tagged with
    /// `&source` (a long-lived `FileLineSource`, typically owned by the
    /// caller's sink). The synchronous `Parse(path)` routes through this
    /// virtual so the emitted line pointers never alias a stack-local
    /// borrowing wrapper.
    virtual void
    ParseStreaming(FileLineSource &source, LogParseSink &sink, ParserOptions options = {}) const = 0;

    /// Live-tail streaming entry point: emits `LogLine`s tagged with
    /// `&source` (a long-lived `StreamLineSource`, typically owned by
    /// the caller's `LogTable`). Each emitted line carries the 1-based
    /// monotonic id assigned by `StreamLineSource::AppendLine` so the
    /// model layer can resolve the row's raw bytes via the source long
    /// after parsing has moved on.
    virtual void
    ParseStreaming(StreamLineSource &source, LogParseSink &sink, ParserOptions options = {}) const = 0;

    /// Renders a parsed line back to the parser's native text form.
    virtual std::string ToString(const LogLine &line) const = 0;
};

} // namespace loglib
