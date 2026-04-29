#include "loglib/log_parser.hpp"

#include "loglib/internal/buffering_sink.hpp"
#include "loglib/log_file.hpp"

#include <fmt/format.h>

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <utility>

namespace loglib
{

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

    auto logFile = std::make_unique<LogFile>(file);
    LogFile *logFilePtr = logFile.get();
    internal::BufferingSink sink(std::move(logFile));

    ParseStreaming(*logFilePtr, sink, ParserOptions{});

    LogData data = sink.TakeData();
    std::vector<std::string> errors = sink.TakeErrors();
    return ParseResult{std::move(data), std::move(errors)};
}

} // namespace loglib
