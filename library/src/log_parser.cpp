#include "loglib/log_parser.hpp"

#include "loglib/internal/buffering_sink.hpp"
#include "loglib/log_file.hpp"
#include "loglib/mapped_file_source.hpp"

#include <fmt/format.h>

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <utility>

namespace loglib
{

void LogParser::ParseStreaming(LogFile &file, StreamingLogSink &sink, ParserOptions options) const
{
    // Borrow @p file via a stack-local `MappedFileSource`, then forward to
    // the source-based virtual. Backwards-compat shim for callers that
    // already own a `LogFile` (the static `File → Open…` path, the
    // synchronous `Parse(path)` path, tests).
    MappedFileSource source(file);
    ParseStreaming(source, sink, std::move(options));
}

ParseResult LogParser::Parse(const std::filesystem::path &file) const
{
    if (!std::filesystem::exists(file))
    {
        throw std::runtime_error(fmt::format("File '{}' does not exist.", file.string()));
    }
    if (std::filesystem::file_size(file) == 0)
    {
        throw std::runtime_error(fmt::format("File '{}' is empty.", file.string()));
    }

    // Hold ownership of the `LogFile` in `BufferingSink` (`TakeData()`
    // returns it inside the resulting `LogData`); a borrowing
    // `MappedFileSource` is the parser's view of it, so the synchronous
    // `Parse(path)` path explicitly routes through the same `LogSource`
    // seam used by the streaming/static GUI flows (task 1.5).
    auto logFile = std::make_unique<LogFile>(file);
    LogFile *logFilePtr = logFile.get();
    internal::BufferingSink sink(std::move(logFile));

    MappedFileSource source(*logFilePtr);
    ParseStreaming(source, sink, ParserOptions{});

    LogData data = sink.TakeData();
    std::vector<std::string> errors = sink.TakeErrors();
    return ParseResult{std::move(data), std::move(errors)};
}

} // namespace loglib
