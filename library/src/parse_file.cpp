#include "loglib/parse_file.hpp"

#include "loglib/file_line_source.hpp"
#include "loglib/format_detection.hpp"
#include "loglib/internal/buffering_sink.hpp"
#include "loglib/log_configuration.hpp"
#include "loglib/log_factory.hpp"
#include "loglib/log_file.hpp"
#include "loglib/log_parser.hpp"
#include "loglib/parser_options.hpp"
#include "loglib/parsers/json_parser.hpp"
#include "loglib/parsers/regex_parser.hpp"
#include "loglib/regex_templates.hpp"

#include <fmt/format.h>

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

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
    // Use the shared probe so `ParseFile` and
    // `DetectFormatForPath` agree on the same file.
    const std::string head = ReadProbeHead(file, PROBE_BYTES_BUDGET);
    if (head.empty())
    {
        throw std::runtime_error(fmt::format("Input file '{}' could not be parsed.", file.string()));
    }

    const DetectedFormat detected = DetectFormatFromBytes(head);
    // `DetectFormatFromBytes` returns `Format::Json` both on a
    // successful JSON probe and on the "nothing matched"
    // fallback. Verify the JSON case explicitly so unknown
    // content still surfaces as an unparseable-file error rather
    // than being silently pushed through the JSON parser.
    if (detected.format == LogConfiguration::Source::Format::Json)
    {
        JsonParser probe;
        if (!probe.IsValidBytes(head))
        {
            throw std::runtime_error(fmt::format("Input file '{}' could not be parsed.", file.string()));
        }
    }

    const std::unique_ptr<LogParser> parser = MakeParserForFormat(detected.format, detected.regexPattern);
    return ParseFile(*parser, file);
}
// NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange)

} // namespace loglib
