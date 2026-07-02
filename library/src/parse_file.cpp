#include "loglib/parse_file.hpp"

#include "loglib/file_line_source.hpp"
#include "loglib/internal/buffering_sink.hpp"
#include "loglib/log_factory.hpp"
#include "loglib/log_file.hpp"
#include "loglib/log_parser.hpp"
#include "loglib/parser_options.hpp"
#include "loglib/parsers/regex_parser.hpp"
#include "loglib/regex_templates.hpp"

#include <fmt/format.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

namespace loglib
{

// MSVC's <filesystem> casts a combined bitmask back to __std_fs_stats_flags
// (e.g. _Follow_symlinks | _File_size = 9), and clang's analyzer flags the
// resulting value as out-of-range. False positive originating in stdlib.
// NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange)
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
    return ParseResult{.data = std::move(data), .errors = std::move(errors)};
}

ParseResult ParseFile(const std::filesystem::path &file)
{
    for (int i = 0; i < static_cast<int>(LogFactory::Parser::Count); ++i)
    {
        const auto parserType = static_cast<LogFactory::Parser>(i);

        // `Regex` needs special handling: the factory-built parser is
        // probe-only (no pinned pattern), so a plain
        // `ParseFile(*parser, file)` would surface the "empty pattern"
        // error even though `IsValid` had already identified the
        // matching built-in template. Detect the template here and
        // construct a parser pinned to its pattern instead.
        if (parserType == LogFactory::Parser::Regex)
        {
            if (const std::optional<RegexTemplate> tmpl = DetectRegexTemplate(file); tmpl.has_value())
            {
                RegexParser parser(tmpl->pattern);
                return ParseFile(parser, file);
            }
            continue;
        }

        const std::unique_ptr<LogParser> parser = LogFactory::Create(parserType);
        if (parser->IsValid(file))
        {
            return ParseFile(*parser, file);
        }
    }

    throw std::runtime_error(fmt::format("Input file '{}' could not be parsed.", file.string()));
}
// NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange)

} // namespace loglib
