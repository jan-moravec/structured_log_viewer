#include "loglib/parse_file.hpp"

#include "loglib/file_line_source.hpp"
#include "loglib/internal/buffering_sink.hpp"
#include "loglib/log_factory.hpp"
#include "loglib/log_file.hpp"
#include "loglib/log_parser.hpp"
#include "loglib/parser_options.hpp"

#include <fmt/format.h>

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <utility>

namespace loglib
{

ParseResult ParseFile(const LogParser &parser, const std::filesystem::path &file)
{
    if (!std::filesystem::exists(file))
    {
        throw std::runtime_error(fmt::format("File '{}' does not exist.", file.string()));
    }
    if (std::filesystem::file_size(file) == 0)
    {
        throw std::runtime_error(fmt::format("File '{}' is empty.", file.string()));
    }

    // The sink owns the `FileLineSource` for the duration of the parse;
    // `TakeData()` transfers ownership into `LogData::mSources` so the
    // emitted `LogLine`'s `LineSource *` back-pointers stay valid.
    auto fileSource = std::make_unique<FileLineSource>(std::make_unique<LogFile>(file));
    FileLineSource *sourceRaw = fileSource.get();
    internal::BufferingSink sink(std::move(fileSource));

    parser.ParseStreaming(*sourceRaw, sink, ParserOptions{});

    LogData data = sink.TakeData();
    std::vector<std::string> errors = sink.TakeErrors();
    return ParseResult{std::move(data), std::move(errors)};
}

ParseResult ParseFile(const std::filesystem::path &file)
{
    for (int i = 0; i < static_cast<int>(LogFactory::Parser::Count); ++i)
    {
        const auto parserType = static_cast<LogFactory::Parser>(i);
        const std::unique_ptr<LogParser> parser = LogFactory::Create(parserType);
        if (parser->IsValid(file))
        {
            return ParseFile(*parser, file);
        }
    }

    throw std::runtime_error(fmt::format("Input file '{}' could not be parsed.", file.string()));
}

} // namespace loglib
