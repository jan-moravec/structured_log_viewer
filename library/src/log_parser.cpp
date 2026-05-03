#include "loglib/log_parser.hpp"

#include "loglib/file_line_source.hpp"
#include "loglib/internal/buffering_sink.hpp"
#include "loglib/log_file.hpp"

#include <fmt/format.h>

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <utility>

namespace loglib
{

void LogParser::ParseStreaming(LogFile &file, StreamingLogSink &sink, ParserOptions options) const
{
    // Borrow @p file via a stack-local `FileLineSource`, then forward
    // to the source-based virtual. Backwards-compat shim for callers
    // that already own a `LogFile` (the static `File → Open…` path,
    // the synchronous `Parse(path)` path, tests).
    FileLineSource source(file);
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

    // Long-lived `FileLineSource` owned by the sink for the duration of
    // the parse and beyond — the emitted `LogLine`s store a raw
    // `LineSource *` back-pointer at it, so it must outlive the lines.
    // `BufferingSink::TakeData()` transfers ownership into `LogData`'s
    // `mSources` so the back-pointers stay valid in the returned result.
    auto fileSource = std::make_unique<FileLineSource>(std::make_unique<LogFile>(file));
    FileLineSource *sourceRaw = fileSource.get();
    internal::BufferingSink sink(std::move(fileSource));

    // Drive the typed static-file overload directly so emitted LogLines
    // reference the long-lived FileLineSource above (not a stack-local
    // borrowing wrapper that would dangle the moment we return).
    ParseStreaming(*sourceRaw, sink, ParserOptions{});

    LogData data = sink.TakeData();
    std::vector<std::string> errors = sink.TakeErrors();
    return ParseResult{std::move(data), std::move(errors)};
}

} // namespace loglib
