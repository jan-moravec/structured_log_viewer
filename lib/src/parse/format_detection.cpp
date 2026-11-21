#include "loglib/parse/format_detection.hpp"

#include "loglib/parse/internal/regex_compiled_pattern.hpp"
#include "loglib/parse/internal/regex_probe.hpp"
#include "loglib/parse/log_parser.hpp"
#include "loglib/parse/parsers/csv_parser.hpp"
#include "loglib/parse/parsers/json_parser.hpp"
#include "loglib/parse/parsers/logfmt_parser.hpp"
#include "loglib/parse/parsers/regex_parser.hpp"

#include <cstddef>
#include <fstream>
#include <ios>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace loglib
{

std::string ReadProbeHead(const std::filesystem::path &file, std::size_t budget)
{
    std::ifstream stream(file, std::ios::binary);
    if (!stream.is_open())
    {
        return {};
    }

    std::string head;
    head.resize(budget);
    stream.read(head.data(), static_cast<std::streamsize>(budget));
    const auto got = stream.gcount();
    if (got <= 0)
    {
        return {};
    }
    head.resize(static_cast<std::size_t>(got));
    return head;
}

std::optional<DetectedFormat> DetectFormat(std::string_view sample, const RegexTemplateCatalog &catalog)
{
    if (sample.empty())
    {
        return std::nullopt;
    }

    // Probe specific formats before generic CSV: JSON, logfmt, regex templates, then CSV.
    if (LooksLikeJson(sample))
    {
        return DetectedFormat{.format = Source::Format::Json};
    }
    if (LooksLikeLogfmt(sample))
    {
        return DetectedFormat{.format = Source::Format::Logfmt};
    }
    if (const RegexTemplate *tmpl = catalog.Detect(sample); tmpl != nullptr)
    {
        return DetectedFormat{
            .format = Source::Format::Regex,
            .regexPattern = tmpl->pattern,
            .regexContinuationMode = tmpl->continuationMode,
            .regexHeaderAnchor = tmpl->headerAnchor,
        };
    }
    if (LooksLikeCsv(sample))
    {
        return DetectedFormat{.format = Source::Format::Csv};
    }
    return std::nullopt;
}

bool SampleFitsFormat(std::string_view sample, Source::Format format, const FormatOptions &options)
{
    switch (format)
    {
    case Source::Format::Json:
        return LooksLikeJson(sample);
    case Source::Format::Logfmt:
        return LooksLikeLogfmt(sample);
    case Source::Format::Csv:
        return LooksLikeCsv(sample);
    case Source::Format::Regex: {
        if (options.regexPattern.empty())
        {
            return false;
        }
        internal::CompiledPattern compiled;
        std::string error;
        if (!compiled.Compile(options.regexPattern, error))
        {
            return false;
        }
        const std::vector<std::string> lines = internal::CollectProbeLines(sample);
        return internal::PatternFitsLines(compiled, lines);
    }
    }
    return false;
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
