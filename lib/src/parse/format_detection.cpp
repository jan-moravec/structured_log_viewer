#include "loglib/parse/format_detection.hpp"

#include "loglib/parse/log_parser.hpp"
#include "loglib/parse/parsers/csv_parser.hpp"
#include "loglib/parse/parsers/json_parser.hpp"
#include "loglib/parse/parsers/logfmt_parser.hpp"
#include "loglib/parse/parsers/regex_parser.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace loglib
{

std::optional<DetectedFormat> TryDetectFormatFromBytes(std::string_view sniffBuffer)
{
    if (sniffBuffer.empty())
    {
        return std::nullopt;
    }

    // Probe specific formats before generic CSV: JSON, logfmt,
    // regex templates, then CSV.
    for (const Source::Format format : {Source::Format::Json, Source::Format::Logfmt})
    {
        if (MakeParserForFormat(format)->IsValidBytes(sniffBuffer))
        {
            return DetectedFormat{.format = format, .regexPattern = {}};
        }
    }
    if (std::optional<RegexTemplate> tmpl = DetectRegexTemplateFromBytes(sniffBuffer); tmpl.has_value())
    {
        return DetectedFormat{.format = Source::Format::Regex, .regexPattern = std::move(tmpl->pattern)};
    }
    if (MakeParserForFormat(Source::Format::Csv)->IsValidBytes(sniffBuffer))
    {
        return DetectedFormat{.format = Source::Format::Csv, .regexPattern = {}};
    }
    return std::nullopt;
}

DetectedFormat DetectFormatFromBytes(std::string_view sniffBuffer)
{
    return TryDetectFormatFromBytes(sniffBuffer).value_or(DetectedFormat{});
}

DetectedFormat DetectFormatForPath(const std::filesystem::path &file)
{
    const std::string head = ReadProbeHead(file, PROBE_BYTES_BUDGET);
    if (head.empty())
    {
        return {};
    }
    return DetectFormatFromBytes(head);
}

std::unique_ptr<LogParser> MakeParserForFormat(Source::Format format, std::string_view regexPattern)
{
    switch (format)
    {
    case Source::Format::Logfmt:
        return std::make_unique<LogfmtParser>();
    case Source::Format::Csv:
        return std::make_unique<CsvParser>();
    case Source::Format::Regex:
        // Pin the pattern on the parser instance directly rather
        // than relying on `ParserOptions::configuration->source->
        // regexPattern`: some callers pass an unrelated snapshot,
        // and the explicit-pattern ctor short-circuits the lookup.
        return std::make_unique<RegexParser>(std::string(regexPattern));
    case Source::Format::Json:
        return std::make_unique<JsonParser>();
    }
    return std::make_unique<JsonParser>();
}

} // namespace loglib
