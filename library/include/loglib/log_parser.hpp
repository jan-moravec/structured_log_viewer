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
class LogFile;
class StreamingLogSink;
class StreamLineSource;

/// Buffered output of a synchronous parse.
struct ParseResult
{
    LogData data;
    std::vector<std::string> errors;
};

/// Base class for log-format parsers. New formats implement `IsValid`,
/// the two `ParseStreaming(LineSource&, ...)` virtuals, and `ToString`.
/// The convenience entry points (`Parse(path)` and the `LogFile&`
/// `ParseStreaming` shim) are non-virtual wrappers that route through
/// the static-file virtual.
class LogParser
{
public:
    virtual ~LogParser() = default;

    virtual bool IsValid(const std::filesystem::path &file) const = 0;

    /// Synchronous parse: routes through `ParseStreaming` into a `BufferingSink`.
    ParseResult Parse(const std::filesystem::path &file) const;

    /// Static-file streaming entry point: emits `LogLine`s tagged with
    /// `&source` (a long-lived `FileLineSource`, typically owned by the
    /// caller's sink). The synchronous `Parse(path)` routes through this
    /// virtual so the emitted line pointers never alias a stack-local
    /// borrowing wrapper.
    virtual void
    ParseStreaming(FileLineSource &source, StreamingLogSink &sink, ParserOptions options = {}) const = 0;

    /// Live-tail streaming entry point: emits `LogLine`s tagged with
    /// `&source` (a long-lived `StreamLineSource`, typically owned by
    /// the caller's `LogTable`). Each emitted line carries the 1-based
    /// monotonic id assigned by `StreamLineSource::AppendLine` so the
    /// model layer can resolve the row's raw bytes via the source long
    /// after parsing has moved on (PRD 4.10.4).
    virtual void
    ParseStreaming(StreamLineSource &source, StreamingLogSink &sink, ParserOptions options = {}) const = 0;

    /// Backward-compatible overload for the static-file path: wraps @p
    /// file in a borrowing `FileLineSource` and forwards to the
    /// source-based virtual. The caller retains ownership of @p file;
    /// the wrapper is stack-local.
    void ParseStreaming(LogFile &file, StreamingLogSink &sink, ParserOptions options = {}) const;

    /// Renders a parsed line back to the parser's native text form.
    virtual std::string ToString(const LogLine &line) const = 0;
};

} // namespace loglib
