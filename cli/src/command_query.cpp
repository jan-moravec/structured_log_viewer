#include "commands.hpp"

#include "console.hpp"
#include "diagnostics.hpp"
#include "load_table.hpp"
#include "session_config.hpp"

#include <loglib/output/exports/export_sink.hpp>
#include <loglib/output/exports/row_exporter.hpp>
#include <loglib/query/leaf_rule_compile.hpp>
#include <loglib/query/log_compare.hpp>
#include <loglib/query/log_filter.hpp>
#include <loglib/query/row_ordering.hpp>
#include <loglib/table/log_table.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace slp
{

namespace
{

[[nodiscard]] loglib::exports::ExportFormat ToExportFormat(QueryFormat format) noexcept
{
    switch (format)
    {
    case QueryFormat::Jsonl:
        return loglib::exports::ExportFormat::JsonLines;
    case QueryFormat::Csv:
        return loglib::exports::ExportFormat::Csv;
    case QueryFormat::Markdown:
        return loglib::exports::ExportFormat::Markdown;
    case QueryFormat::Snapshot:
        return loglib::exports::ExportFormat::Snapshot;
    }
    return loglib::exports::ExportFormat::JsonLines;
}

/** @brief The part of the rows that a query shows, as positions from begin up to end. */
struct RowWindow
{
    std::size_t begin = 0;
    std::size_t end = 0;
};

/** @brief The part of @p rowCount rows that the last option, or the offset and limit options, select. */
[[nodiscard]] RowWindow PageWindow(const QueryOptions &options, std::size_t rowCount)
{
    if (options.last)
    {
        const auto keep = static_cast<std::size_t>(std::min<std::uint64_t>(*options.last, rowCount));
        return {.begin = rowCount - keep, .end = rowCount};
    }
    std::size_t begin = 0;
    if (options.offset)
    {
        begin = static_cast<std::size_t>(std::min<std::uint64_t>(*options.offset, rowCount));
    }
    std::size_t end = rowCount;
    if (options.limit)
    {
        end = begin + static_cast<std::size_t>(std::min<std::uint64_t>(*options.limit, rowCount - begin));
    }
    return {.begin = begin, .end = end};
}

/** @brief Keeps only the rows of @p window, in their order. */
void ApplyWindow(const RowWindow &window, std::vector<std::size_t> &rows)
{
    rows.erase(rows.begin() + static_cast<std::ptrdiff_t>(window.end), rows.end());
    rows.erase(rows.begin(), rows.begin() + static_cast<std::ptrdiff_t>(window.begin));
}

/** @brief Replaces @p rows by the rows of @p window in the session sort order. The rest is not sorted. */
void SortRows(
    const loglib::LogTable &table, const SortChoice &sort, const RowWindow &window, std::vector<std::size_t> &rows
)
{
    const auto &columns = table.Configuration().Configuration().columns;
    std::optional<loglib::EnumDictRank> rankStorage;
    const loglib::EnumDictRank *rank = nullptr;
    if (columns[sort.columnIndex].type == loglib::ColumnType::Enumeration)
    {
        const auto lookup = table.ResolveEnumColumn(sort.columnIndex);
        if (lookup.dictionary != nullptr)
        {
            rankStorage.emplace(*lookup.dictionary);
            rank = &*rankStorage;
        }
    }
    const loglib::RowOrdering ordering(sort.columnIndex, sort.direction, rank);
    // The permutation holds positions within rows, not row indices. It is turned into the rows in place.
    std::vector<std::size_t> sorted =
        ordering.PermuteWindow(table, std::span<const std::size_t>{rows}, window.begin, window.end);
    for (std::size_t &position : sorted)
    {
        position = rows[position];
    }
    rows = std::move(sorted);
}

/** @brief Exports @p plan to the output option's file or to stdout. A failed write is an output failure. */
[[nodiscard]] std::optional<Failure> WriteRows(
    const QueryOptions &options,
    const Console &console,
    const RuntimeContext &runtime,
    loglib::exports::ExportPlan &plan
)
{
    const auto exporter = loglib::exports::MakeExporter(plan.format);
    if (!exporter)
    {
        return console.Fail(ExitCode::Usage, "unsupported export format.");
    }

    const bool toStdout = options.output == "-" || options.output.empty();
    if (!toStdout)
    {
        std::error_code ec;
        if (std::filesystem::exists(options.output, ec) && !options.force)
        {
            return console.Fail(
                ExitCode::Output, std::format("output '{}' already exists; pass --force to overwrite.", options.output)
            );
        }
    }
    try
    {
        if (toStdout)
        {
            // A closed pipe gives BrokenPipe, which main treats as success.
            loglib::exports::StreamSink sink(console.Out());
            exporter->Run(plan.View(), sink, runtime.stop);
            sink.Finish();
        }
        else
        {
            loglib::exports::FileSink sink(options.output);
            exporter->Run(plan.View(), sink, runtime.stop);
            sink.Finish();
        }
    }
    catch (const std::runtime_error &ex)
    {
        return console.Fail(ExitCode::Output, ex.what());
    }
    return std::nullopt;
}

} // namespace

ExitCode RunQuery(
    const QueryOptions &options, const GlobalOptions &global, Console &console, const RuntimeContext &runtime
)
{
    const auto parseStarted = std::chrono::steady_clock::now();
    auto loaded = LoadTable(global, options.input, console, runtime);
    if (!loaded)
    {
        return console.Report(loaded.error());
    }
    loglib::LogTable &table = loaded->table;
    console.Stage("parse", table.RowCount(), parseStarted);

    auto session = ApplySessionConfiguration(options.filter, options.view, table, runtime, QueryOptions::NAME);
    if (!session)
    {
        return console.Report(session.error());
    }
    if (auto failure = CheckLoadedFilter(options.filter, *session, table, QueryOptions::NAME))
    {
        return console.Report(*failure);
    }

    const auto filterStarted = std::chrono::steady_clock::now();
    const auto compiled =
        loglib::CompileExpression(session->expression, table.Configuration().Configuration().columns, &table);
    std::vector<std::size_t> rows = loglib::FilterAcceptedRows(table, compiled);
    console.Stage("filter", rows.size(), filterStarted);

    // A sorted query that shows only some rows sorts only those.
    const RowWindow window = PageWindow(options, rows.size());
    if (session->sort)
    {
        const auto sortStarted = std::chrono::steady_clock::now();
        const std::size_t sortedRows = rows.size();
        SortRows(table, *session->sort, window, rows);
        console.Stage("sort", sortedRows, sortStarted);
    }
    else
    {
        ApplyWindow(window, rows);
    }
    console.ParseErrors(loaded->parseErrors);

    // Saved configuration is the effective one; it does not depend on paging or output.
    if (auto failure = SaveSessionFiles(options.save, options.input, global, *loaded, *session))
    {
        return console.Report(*failure);
    }

    const bool empty = rows.empty();
    if (options.count)
    {
        if (const ExitCode written = console.Print(std::to_string(rows.size()) + '\n'); written != ExitCode::Success)
        {
            return written;
        }
    }
    else
    {
        if (options.noHeader && options.outputFormat != QueryFormat::Csv &&
            options.outputFormat != QueryFormat::Markdown)
        {
            console.Warn("'--no-header' has no effect for this output format");
        }

        loglib::exports::ExportPlan plan;
        plan.format = ToExportFormat(options.outputFormat);
        plan.sourceRows = std::move(rows);
        plan.visibleColumns = session->visibleColumns;
        plan.includeAllFieldsForJson = !options.jsonVisibleOnly;
        plan.includeHeaderRow = !options.noHeader;
        plan.table = &table;
        const auto exportStarted = std::chrono::steady_clock::now();
        const std::size_t exportRows = plan.sourceRows.size();
        if (auto failure = WriteRows(options, console, runtime, plan))
        {
            return console.Report(*failure);
        }
        console.Stage("export", exportRows, exportStarted);
    }

    if (options.strict && !loaded->parseErrors.empty())
    {
        return ExitCode::Data;
    }
    return empty && !options.allowEmpty ? ExitCode::Empty : ExitCode::Success;
}

} // namespace slp
