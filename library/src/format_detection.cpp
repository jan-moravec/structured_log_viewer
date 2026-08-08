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

    // Probe order (specific-before-generic):
    //  1. JSON  -- anchored on `{` / `[` at the first non-whitespace
    //     byte, essentially free.
    //  2. Logfmt -- anchored on `key=` shape, also cheap.
    //  3. Regex templates -- each ships an anchored PCRE pattern
    //     matched against the first non-blank lines. This tier
    //     runs BEFORE CSV because CSV's `IsValidBytes` is
    //     satisfied by any input with a consistent per-line comma
    //     count, which false-positives on formats whose timestamp
    //     uses `,SSS` for milliseconds (Java Logback, log4j2,
    //     SLF4J default PatternLayout). Regex-template probes are
    //     bounded to `PROBE_BYTES_BUDGET` bytes and the compiled
    //     patterns are process-lifetime-cached, so this ordering
    //     costs a fixed handful of microseconds per open.
    //  4. CSV -- last-resort generic fallback for a comma-separated
    //     shape that none of the shipped templates recognise.
    struct Probe
    {
        LogFactory::Parser parser;
        LogConfiguration::Source::Format format;
    };
    static constexpr Probe HEAD_PROBES[] = {
        {LogFactory::Parser::Json, LogConfiguration::Source::Format::Json},
        {LogFactory::Parser::Logfmt, LogConfiguration::Source::Format::Logfmt},
    };
    for (const Probe &probe : HEAD_PROBES)
    {
        const std::unique_ptr<LogParser> instance = LogFactory::Create(probe.parser);
        if (instance->IsValidBytes(sniffBuffer))
        {
            return {.format = probe.format, .regexPattern = {}};
        }
    }
    if (std::optional<RegexTemplate> tmpl = DetectRegexTemplateFromBytes(sniffBuffer); tmpl.has_value())
    {
        return {.format = LogConfiguration::Source::Format::Regex, .regexPattern = std::move(tmpl->pattern)};
    }
    if (const std::unique_ptr<LogParser> csv = LogFactory::Create(LogFactory::Parser::Csv); csv->IsValidBytes(sniffBuffer))
    {
        return {.format = LogConfiguration::Source::Format::Csv, .regexPattern = {}};
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
