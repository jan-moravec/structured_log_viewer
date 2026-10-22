#include "commands.hpp"

#include "console.hpp"
#include "diagnostics.hpp"
#include "load_table.hpp"
#include "text_util.hpp"

#include <loglib/column_type_health.hpp>
#include <loglib/parse_error_text.hpp>

#include <algorithm>
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

[[nodiscard]] std::string_view DetectedFormatName(const LoadedTable &loaded) noexcept
{
    return loaded.format ? FormatName(loaded.format->format) : "unknown";
}

[[nodiscard]] double ErrorRatePercent(std::uint64_t errors, std::uint64_t lines) noexcept
{
    if (lines == 0)
    {
        return errors == 0 ? 0.0 : 100.0;
    }
    return (100.0 * static_cast<double>(errors)) / static_cast<double>(lines);
}

[[nodiscard]] bool WithinErrorBudget(const ValidateOptions &options, std::uint64_t errors, std::uint64_t lines) noexcept
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

[[nodiscard]] std::string RenderText(const LoadedTable &loaded, const ValidateOptions &options)
{
    const std::uint64_t rows = loaded.table.RowCount();
    const std::uint64_t errors = loaded.parseErrors.size();
    const std::uint64_t lines = loaded.linesConsumed == 0 ? rows + errors : loaded.linesConsumed;
    const double rate = ErrorRatePercent(errors, lines);

    std::ostringstream out;
    out << "format: " << DetectedFormatName(loaded) << '\n';
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

[[nodiscard]] std::string RenderJson(const LoadedTable &loaded, const ValidateOptions &options)
{
    const std::uint64_t rows = loaded.table.RowCount();
    const std::uint64_t errors = loaded.parseErrors.size();
    const std::uint64_t lines = loaded.linesConsumed == 0 ? rows + errors : loaded.linesConsumed;
    const double rate = ErrorRatePercent(errors, lines);

    std::string out = "{\n  \"format\": ";
    AppendJsonString(out, DetectedFormatName(loaded));
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
        const loglib::ParseErrorLocation location = loglib::LocateParseError(loaded.parseErrors[i]);
        out += "    {\n";
        if (location.firstLine != 0)
        {
            out += "      \"line\": ";
            out += std::to_string(location.firstLine);
            out += ",\n";
        }
        if (location.lastLine != location.firstLine)
        {
            out += "      \"lastLine\": ";
            out += std::to_string(location.lastLine);
            out += ",\n";
        }
        out += "      \"message\": ";
        AppendJsonString(out, location.message);
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

ExitCode RunValidate(
    const ValidateOptions &options, const GlobalOptions &global, Console &console, const RuntimeContext &runtime
)
{
    const auto loaded = LoadTable(global, options.input, console, runtime);
    if (!loaded)
    {
        return console.Report(loaded.error());
    }
    console.Print(
        options.outputFormat == ReportFormat::Json ? RenderJson(*loaded, options) : RenderText(*loaded, options)
    );

    const std::uint64_t rows = loaded->table.RowCount();
    const std::uint64_t errors = loaded->parseErrors.size();
    const std::uint64_t lines = loaded->linesConsumed == 0 ? rows + errors : loaded->linesConsumed;
    return WithinErrorBudget(options, errors, lines) ? ExitCode::Success : ExitCode::Data;
}

} // namespace slp
