#include "commands.hpp"

#include "console.hpp"
#include "diagnostics.hpp"
#include "json_report.hpp"
#include "load_table.hpp"

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

// Report structs have external linkage: glaze reflects member names
// through templates that MSVC cannot instantiate for internal types.
namespace reports
{
// NOLINTBEGIN(misc-use-internal-linkage)

/** @brief One reported parse error. `line` and `lastLine` are 1-based. */
struct ValidateError
{
    std::optional<std::uint64_t> line;
    /** @brief Set only when the error spans several lines. */
    std::optional<std::uint64_t> lastLine;
    std::string message;
};

/** @brief Type agreement of one column's values. */
struct ColumnHealth
{
    std::string header;
    std::uint64_t totalSlots = 0;
    std::uint64_t presentSlots = 0;
    std::uint64_t matchingSlots = 0;
    bool typeMismatch = false;
};

/** @brief The `slp validate` report. Its members are the JSON keys. */
struct ValidateReport
{
    std::string format;
    std::uint64_t linesConsumed = 0;
    std::uint64_t rowsProduced = 0;
    std::uint64_t parseErrorCount = 0;
    double errorRatePercent = 0.0;
    /** @brief At most `--max-errors` entries. */
    std::vector<ValidateError> errors;
    std::vector<ColumnHealth> columnHealth;
};

// NOLINTEND(misc-use-internal-linkage)

} // namespace reports

namespace
{

using reports::ColumnHealth;
using reports::ValidateError;
using reports::ValidateReport;

[[nodiscard]] double ErrorRatePercent(std::uint64_t errors, std::uint64_t lines) noexcept
{
    if (lines == 0)
    {
        return errors == 0 ? 0.0 : 100.0;
    }
    return (100.0 * static_cast<double>(errors)) / static_cast<double>(lines);
}

[[nodiscard]] ValidateReport Assess(const LoadedTable &loaded, std::uint64_t maxErrors)
{
    ValidateReport report;
    report.format = loaded.format ? std::string(FormatName(loaded.format->format)) : "unknown";
    report.rowsProduced = loaded.table.RowCount();
    report.parseErrorCount = loaded.parseErrors.size();
    report.linesConsumed =
        loaded.linesConsumed == 0 ? report.rowsProduced + report.parseErrorCount : loaded.linesConsumed;
    report.errorRatePercent = ErrorRatePercent(report.parseErrorCount, report.linesConsumed);

    const auto listed = static_cast<std::size_t>(std::min<std::uint64_t>(report.parseErrorCount, maxErrors));
    for (std::size_t index = 0; index < listed; ++index)
    {
        const loglib::ParseErrorLocation location = loglib::LocateParseError(loaded.parseErrors[index]);
        ValidateError error{.line = std::nullopt, .lastLine = std::nullopt, .message = std::string(location.message)};
        if (location.firstLine != 0)
        {
            error.line = location.firstLine;
        }
        if (location.lastLine != location.firstLine)
        {
            error.lastLine = location.lastLine;
        }
        report.errors.push_back(std::move(error));
    }

    const auto &columns = loaded.table.Configuration().Configuration().columns;
    for (std::size_t index = 0; index < columns.size(); ++index)
    {
        const auto health = loaded.table.ComputeColumnTypeHealth(index);
        report.columnHealth.push_back(
            ColumnHealth{
                .header = columns[index].header,
                .totalSlots = health.totalSlots,
                .presentSlots = health.presentSlots,
                .matchingSlots = health.matchingSlots,
                .typeMismatch = health.presentSlots > health.matchingSlots,
            }
        );
    }
    return report;
}

[[nodiscard]] std::string RenderText(const ValidateReport &report, const LoadedTable &loaded)
{
    std::ostringstream out;
    out << "format: " << report.format << '\n';
    out << "lines: " << report.linesConsumed << '\n';
    out << "rows: " << report.rowsProduced << '\n';
    out << "errors: " << report.parseErrorCount << '\n';
    out << "errorRatePercent: " << report.errorRatePercent << '\n';

    bool mismatch = false;
    for (const ColumnHealth &health : report.columnHealth)
    {
        if (health.typeMismatch)
        {
            mismatch = true;
            out << "columnHealth: " << health.header << " present=" << health.presentSlots
                << " matching=" << health.matchingSlots << " (type mismatch)\n";
        }
    }
    if (!mismatch && !report.columnHealth.empty())
    {
        out << "columnHealth: ok\n";
    }

    // Text mode prints each error as the parser worded it.
    for (std::size_t index = 0; index < report.errors.size(); ++index)
    {
        out << loaded.parseErrors[index] << '\n';
    }
    if (report.parseErrorCount > report.errors.size())
    {
        out << "... " << (report.parseErrorCount - report.errors.size()) << " more error(s) omitted\n";
    }
    return out.str();
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
    const ValidateReport report = Assess(*loaded, options.maxErrors);
    if (const ExitCode written = console.Print(
            options.outputFormat == ReportFormat::Json ? WriteJsonReport(report) : RenderText(report, *loaded)
        );
        written != ExitCode::Success)
    {
        return written;
    }

    const bool withinBudget =
        report.parseErrorCount == 0 || (options.maxErrorRate && report.errorRatePercent <= *options.maxErrorRate);
    return withinBudget ? ExitCode::Success : ExitCode::Data;
}

} // namespace slp
