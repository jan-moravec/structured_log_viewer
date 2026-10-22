#include "commands.hpp"

#include "diagnostics.hpp"
#include "load_table.hpp"

#include <loglib/column_type_health.hpp>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace slp
{

namespace
{

struct ParsedError
{
    std::optional<std::uint64_t> line;
    std::string message;
};

[[nodiscard]] ParsedError SplitParseError(std::string_view text)
{
    ParsedError parsed;
    parsed.message = std::string(text);
    constexpr std::string_view PREFIX = "Error on line ";
    if (!text.starts_with(PREFIX))
    {
        return parsed;
    }
    const std::size_t begin = PREFIX.size();
    const std::size_t end = text.find_first_not_of("0123456789", begin);
    if (end == begin || end == std::string_view::npos || text[end] != ':')
    {
        return parsed;
    }
    std::uint64_t line = 0;
    const auto [ptr, ec] = std::from_chars(text.data() + begin, text.data() + end, line);
    if (ec != std::errc{} || ptr != text.data() + end)
    {
        return parsed;
    }
    parsed.line = line;
    std::string_view rest = text.substr(end + 1);
    while (!rest.empty() && rest.front() == ' ')
    {
        rest.remove_prefix(1);
    }
    parsed.message = std::string(rest);
    return parsed;
}

void AppendJsonString(std::string &out, std::string_view text)
{
    out.push_back('"');
    for (const unsigned char ch : text)
    {
        switch (ch)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (ch < 0x20)
            {
                constexpr char HEX[] = "0123456789abcdef";
                out += "\\u00";
                out.push_back(HEX[ch >> 4]);
                out.push_back(HEX[ch & 0x0f]);
            }
            else
            {
                out.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    out.push_back('"');
}

[[nodiscard]] double ErrorRatePercent(std::uint64_t errors, std::uint64_t lines) noexcept
{
    if (lines == 0)
    {
        return errors == 0 ? 0.0 : 100.0;
    }
    return (100.0 * static_cast<double>(errors)) / static_cast<double>(lines);
}

[[nodiscard]] bool WithinErrorBudget(const Options &options, std::uint64_t errors, std::uint64_t lines) noexcept
{
    if (errors == 0)
    {
        return true;
    }
    if (!options.maxErrorRate)
    {
        return false;
    }
    return ErrorRatePercent(errors, lines) <= *options.maxErrorRate;
}

[[nodiscard]] std::string RenderText(const LoadedTable &loaded, const Options &options)
{
    const std::uint64_t rows = loaded.table.RowCount();
    const std::uint64_t errors = loaded.parseErrors.size();
    const std::uint64_t lines = loaded.linesConsumed == 0 ? rows + errors : loaded.linesConsumed;
    const double rate = ErrorRatePercent(errors, lines);

    std::ostringstream out;
    out << "format: " << (loaded.detectedFormat.empty() ? "unknown" : loaded.detectedFormat) << '\n';
    out << "lines: " << lines << '\n';
    out << "rows: " << rows << '\n';
    out << "errors: " << errors << '\n';
    out << "errorRatePercent: " << rate << '\n';

    const auto &columns = loaded.table.Configuration().Configuration().columns;
    std::size_t demoted = 0;
    for (std::size_t i = 0; i < columns.size(); ++i)
    {
        const auto health = loaded.table.ComputeColumnTypeHealth(i);
        if (health.presentSlots > health.matchingSlots)
        {
            ++demoted;
            out << "columnHealth: " << columns[i].header << " present=" << health.presentSlots
                << " matching=" << health.matchingSlots << " (type mismatch)\n";
        }
    }
    if (demoted == 0 && !columns.empty())
    {
        out << "columnHealth: ok\n";
    }

    const std::uint64_t limit = options.maxErrors;
    for (std::size_t i = 0; i < loaded.parseErrors.size() && i < limit; ++i)
    {
        out << loaded.parseErrors[i] << '\n';
    }
    if (loaded.parseErrors.size() > limit)
    {
        out << "... " << (loaded.parseErrors.size() - limit) << " more error(s) omitted\n";
    }
    return out.str();
}

[[nodiscard]] std::string RenderJson(const LoadedTable &loaded, const Options &options)
{
    const std::uint64_t rows = loaded.table.RowCount();
    const std::uint64_t errors = loaded.parseErrors.size();
    const std::uint64_t lines = loaded.linesConsumed == 0 ? rows + errors : loaded.linesConsumed;
    const double rate = ErrorRatePercent(errors, lines);

    std::string out = "{\n  \"format\": ";
    AppendJsonString(out, loaded.detectedFormat.empty() ? "unknown" : loaded.detectedFormat);
    out += ",\n  \"linesConsumed\": ";
    out += std::to_string(lines);
    out += ",\n  \"rowsProduced\": ";
    out += std::to_string(rows);
    out += ",\n  \"parseErrorCount\": ";
    out += std::to_string(errors);
    out += ",\n  \"errorRatePercent\": ";
    out += std::to_string(rate);
    out += ",\n  \"errors\": [\n";

    const std::uint64_t limit = options.maxErrors;
    const std::size_t emit = static_cast<std::size_t>(std::min<std::uint64_t>(errors, limit));
    for (std::size_t i = 0; i < emit; ++i)
    {
        if (i != 0)
        {
            out += ",\n";
        }
        const ParsedError parsed = SplitParseError(loaded.parseErrors[i]);
        out += "    {\n";
        if (parsed.line)
        {
            out += "      \"line\": ";
            out += std::to_string(*parsed.line);
            out += ",\n";
        }
        out += "      \"message\": ";
        AppendJsonString(out, parsed.message);
        out += "\n    }";
    }
    out += "\n  ],\n  \"columnHealth\": [\n";

    const auto &columns = loaded.table.Configuration().Configuration().columns;
    for (std::size_t i = 0; i < columns.size(); ++i)
    {
        if (i != 0)
        {
            out += ",\n";
        }
        const auto health = loaded.table.ComputeColumnTypeHealth(i);
        const bool mismatch = health.presentSlots > health.matchingSlots;
        out += "    {\n      \"header\": ";
        AppendJsonString(out, columns[i].header);
        out += ",\n      \"totalSlots\": ";
        out += std::to_string(health.totalSlots);
        out += ",\n      \"presentSlots\": ";
        out += std::to_string(health.presentSlots);
        out += ",\n      \"matchingSlots\": ";
        out += std::to_string(health.matchingSlots);
        out += ",\n      \"typeMismatch\": ";
        out += mismatch ? "true" : "false";
        out += "\n    }";
    }
    out += "\n  ]\n}\n";
    return out;
}

} // namespace

CommandOutput RunValidate(const Options &options, const RuntimeContext &runtime)
{
    auto [loaded, side] = LoadTable(options, runtime);
    CommandOutput out;
    out.err = std::move(side.err);
    if (side.exitCode != 0)
    {
        out.exitCode = side.exitCode;
        return out;
    }

    if (options.outputFormat == "json")
    {
        out.out = RenderJson(loaded, options);
    }
    else
    {
        out.out = RenderText(loaded, options);
    }

    const std::uint64_t rows = loaded.table.RowCount();
    const std::uint64_t errors = loaded.parseErrors.size();
    const std::uint64_t lines = loaded.linesConsumed == 0 ? rows + errors : loaded.linesConsumed;
    out.exitCode = WithinErrorBudget(options, errors, lines) ? static_cast<int>(ExitCode::Success)
                                                             : static_cast<int>(ExitCode::Data);
    return out;
}

} // namespace slp
