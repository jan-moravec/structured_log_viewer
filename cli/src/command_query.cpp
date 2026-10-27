#include "commands.hpp"

#include "console.hpp"
#include "diagnostics.hpp"
#include "load_table.hpp"
#include "session_config.hpp"

#include <loglib/exports/export_sink.hpp>
#include <loglib/exports/row_exporter.hpp>
#include <loglib/leaf_rule_compile.hpp>
#include <loglib/log_compare.hpp>
#include <loglib/log_filter.hpp>
#include <loglib/log_table.hpp>
#include <loglib/row_ordering.hpp>

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

/** @brief Reorders @p rows by the session sort. */
void SortRows(const loglib::LogTable &table, const SortChoice &sort, std::vector<std::size_t> &rows)
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
    // `Permute` returns positions within `rows`, not row indices.
    const std::vector<std::size_t> permutation = ordering.Permute(table, std::span<const std::size_t>{rows});
    std::vector<std::size_t> sorted;
    sorted.reserve(permutation.size());
    for (const std::size_t position : permutation)
    {
        sorted.push_back(rows[position]);
    }
    rows = std::move(sorted);
}

/** @brief Applies `--last`, or `--offset` and `--limit`, to @p rows. */
void PageRows(const QueryOptions &options, std::vector<std::size_t> &rows)
{
    if (options.last)
    {
        if (rows.size() > *options.last)
        {
            rows.erase(rows.begin(), rows.end() - static_cast<std::ptrdiff_t>(*options.last));
        }
        return;
    }
    if (options.offset)
    {
        const auto start = static_cast<std::size_t>(std::min<std::uint64_t>(*options.offset, rows.size()));
        rows.erase(rows.begin(), rows.begin() + static_cast<std::ptrdiff_t>(start));
    }
    if (options.limit && rows.size() > *options.limit)
    {
        rows.resize(static_cast<std::size_t>(*options.limit));
    }
}

/** @brief Exports @p plan to `--output` or stdout. A failed write is an output failure. */
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
            // A closed pipe surfaces as `BrokenPipe`, which `main` treats as success.
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

    const auto filterStarted = std::chrono::steady_clock::now();
    const auto compiled =
        loglib::CompileExpression(session->expression, table.Configuration().Configuration().columns, &table);
    std::vector<std::size_t> rows = loglib::FilterAcceptedRows(table, compiled);
    console.Stage("filter", rows.size(), filterStarted);

    if (session->sort)
    {
        SortRows(table, *session->sort, rows);
    }
    PageRows(options, rows);
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
        if (auto failure = WriteRows(options, console, runtime, plan))
        {
            return console.Report(*failure);
        }
    }

    if (options.strict && !loaded->parseErrors.empty())
    {
        return ExitCode::Data;
    }
    return empty && !options.allowEmpty ? ExitCode::Empty : ExitCode::Success;
}

} // namespace slp
