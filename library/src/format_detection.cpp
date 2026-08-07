#include "loglib/format_detection.hpp"

#include "loglib/log_factory.hpp"
#include "loglib/log_parser.hpp"
#include "loglib/parsers/csv_parser.hpp"
#include "loglib/parsers/json_parser.hpp"
#include "loglib/parsers/logfmt_parser.hpp"
#include "loglib/parsers/regex_parser.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace loglib
{

DetectedFormat DetectFormatFromBytes(std::string_view sniffBuffer)
{
    if (sniffBuffer.empty())
    {
        return {};
    }

    for (int i = 0; i < static_cast<int>(LogFactory::Parser::Count); ++i)
    {
        const auto parserType = static_cast<LogFactory::Parser>(i);
        if (parserType == LogFactory::Parser::Regex)
        {
            // Special-cased like `loglib::ParseFile(path)`: we need
            // the matched template's pattern, not a bare yes/no.
            if (std::optional<RegexTemplate> tmpl = DetectRegexTemplateFromBytes(sniffBuffer); tmpl.has_value())
            {
                return {.format = LogConfiguration::Source::Format::Regex, .regexPattern = std::move(tmpl->pattern)};
            }
            continue;
        }

        const std::unique_ptr<LogParser> probe = LogFactory::Create(parserType);
        if (probe->IsValidBytes(sniffBuffer))
        {
            switch (parserType)
            {
            case LogFactory::Parser::Logfmt:
                return {.format = LogConfiguration::Source::Format::Logfmt};
            case LogFactory::Parser::Csv:
                return {.format = LogConfiguration::Source::Format::Csv};
            case LogFactory::Parser::Json:
            case LogFactory::Parser::Regex:
            case LogFactory::Parser::Count:
                return {.format = LogConfiguration::Source::Format::Json};
            }
        }
    }
    return {};
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

std::unique_ptr<LogParser> MakeParserForFormat(
    LogConfiguration::Source::Format format, std::string_view regexPattern
)
{
    switch (format)
    {
    case LogConfiguration::Source::Format::Logfmt:
        return std::make_unique<LogfmtParser>();
    case LogConfiguration::Source::Format::Csv:
        return std::make_unique<CsvParser>();
    case LogConfiguration::Source::Format::Regex:
        // Pin the pattern on the parser instance directly rather
        // than relying on `ParserOptions::configuration->source->
        // regexPattern`: some callers pass an unrelated snapshot,
        // and the explicit-pattern ctor short-circuits the lookup.
        return std::make_unique<RegexParser>(std::string(regexPattern));
    case LogConfiguration::Source::Format::Json:
        return std::make_unique<JsonParser>();
    }
    return std::make_unique<JsonParser>();
}

} // namespace loglib
