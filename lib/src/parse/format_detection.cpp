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
            return DetectedFormat{.format = format};
        }
    }
    if (std::optional<RegexTemplate> tmpl = DetectRegexTemplateFromBytes(sniffBuffer); tmpl.has_value())
    {
        return DetectedFormat{
            .format = Source::Format::Regex,
            .regexPattern = std::move(tmpl->pattern),
            .regexContinuationMode = tmpl->continuationMode,
            .regexHeaderAnchor = std::move(tmpl->headerAnchor),
        };
    }
    if (MakeParserForFormat(Source::Format::Csv)->IsValidBytes(sniffBuffer))
    {
        return DetectedFormat{.format = Source::Format::Csv};
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

std::unique_ptr<LogParser> MakeParserForFormat(Source::Format format, const FormatOptions &options)
{
    switch (format)
    {
    case Source::Format::Logfmt:
        return std::make_unique<LogfmtParser>(options.multilineLogfmt);
    case Source::Format::Csv:
        return std::make_unique<CsvParser>();
    case Source::Format::Regex:
        return std::make_unique<RegexParser>(
            options.regexPattern, options.regexContinuationMode, options.regexHeaderAnchor
        );
    case Source::Format::Json:
        return std::make_unique<JsonParser>();
    }
    return std::make_unique<JsonParser>();
}

} // namespace loglib
