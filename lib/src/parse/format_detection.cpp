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

std::optional<FormatSpec> DetectFormat(std::string_view sample, const RegexTemplateCatalog &catalog)
{
    if (sample.empty())
    {
        return std::nullopt;
    }

    // Probe specific formats before generic CSV: JSON, logfmt, regex templates, then CSV.
    if (LooksLikeJson(sample))
    {
        return FormatSpec{.format = LogFormat::Json};
    }
    if (LooksLikeLogfmt(sample))
    {
        return FormatSpec{.format = LogFormat::Logfmt};
    }
    if (const RegexTemplate *tmpl = catalog.Detect(sample); tmpl != nullptr)
    {
        return FormatSpec{.format = LogFormat::Regex, .regex = tmpl->regex};
    }
    if (LooksLikeCsv(sample))
    {
        return FormatSpec{.format = LogFormat::Csv};
    }
    return std::nullopt;
}

bool SampleFitsFormat(std::string_view sample, const FormatSpec &spec)
{
    switch (spec.format)
    {
    case LogFormat::Json:
        return LooksLikeJson(sample);
    case LogFormat::Logfmt:
        return LooksLikeLogfmt(sample);
    case LogFormat::Csv:
        return LooksLikeCsv(sample);
    case LogFormat::Regex:
    {
        if (spec.regex.pattern.empty())
        {
            return false;
        }
        internal::CompiledPattern compiled;
        std::string error;
        if (!compiled.Compile(spec.regex.pattern, error))
        {
            return false;
        }
        const std::vector<std::string> lines = internal::CollectProbeLines(sample);
        return internal::PatternFitsLines(compiled, lines);
    }
    }
    return false;
}

std::unique_ptr<LogParser> MakeParser(const FormatSpec &spec)
{
    switch (spec.format)
    {
    case LogFormat::Logfmt:
        return std::make_unique<LogfmtParser>(spec.logfmtMultiline);
    case LogFormat::Csv:
        return std::make_unique<CsvParser>();
    case LogFormat::Regex:
        return std::make_unique<RegexParser>(spec.regex);
    case LogFormat::Json:
        return std::make_unique<JsonParser>();
    }
    return std::make_unique<JsonParser>();
}

} // namespace loglib
