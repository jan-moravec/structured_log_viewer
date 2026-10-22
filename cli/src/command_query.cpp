#include "commands.hpp"

#include "cli_filter.hpp"
#include "diagnostics.hpp"
#include "load_table.hpp"
#include "platform.hpp"

#include <loglib/column_projection.hpp>
#include <loglib/decompressing_byte_source.hpp>
#include <loglib/exports/export_sink.hpp>
#include <loglib/exports/row_exporter.hpp>
#include <loglib/file_line_source.hpp>
#include <loglib/filter_expression.hpp>
#include <loglib/format_detection.hpp>
#include <loglib/leaf_rule_compile.hpp>
#include <loglib/log_compare.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_file.hpp>
#include <loglib/log_filter.hpp>
#include <loglib/log_level.hpp>
#include <loglib/log_parse_sink.hpp>
#include <loglib/log_parser.hpp>
#include <loglib/log_table.hpp>
#include <loglib/query_parser.hpp>
#include <loglib/regex_templates.hpp>
#include <loglib/row_ordering.hpp>
#include <loglib/session_bundle.hpp>
#include <loglib/source_plan.hpp>
#include <loglib/stdin_bytes_producer.hpp>
#include <loglib/stdin_peek.hpp>
#include <loglib/stream_line_source.hpp>
#include <loglib/user_paths.hpp>
#include <loglib/user_timestamp.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace slp
{

namespace
{

[[nodiscard]] bool EqualsIgnoreCase(std::string_view lhs, std::string_view rhs) noexcept
{
    if (lhs.size() != rhs.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i)
    {
        if (std::tolower(static_cast<unsigned char>(lhs[i])) != std::tolower(static_cast<unsigned char>(rhs[i])))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] CommandOutput QueryFail(ExitCode code, std::string_view sentence, std::string errExtra = {})
{
    CommandOutput out;
    out.exitCode = static_cast<int>(code);
    out.err = FormatErrorLine("query", sentence);
    out.err.push_back('\n');
    out.err += std::move(errExtra);
    return out;
}

[[nodiscard]] std::optional<loglib::exports::ExportFormat> ToExportFormat(std::string_view token) noexcept
{
    if (token == "jsonl")
    {
        return loglib::exports::ExportFormat::JsonLines;
    }
    if (token == "csv")
    {
        return loglib::exports::ExportFormat::Csv;
    }
    if (token == "markdown")
    {
        return loglib::exports::ExportFormat::Markdown;
    }
    if (token == "snapshot")
    {
        return loglib::exports::ExportFormat::Snapshot;
    }
    return std::nullopt;
}

[[nodiscard]] std::vector<std::string> SplitCsv(std::string_view text)
{
    std::vector<std::string> parts;
    std::size_t begin = 0;
    while (begin <= text.size())
    {
        const std::size_t comma = text.find(',', begin);
        const std::string_view part =
            comma == std::string_view::npos ? text.substr(begin) : text.substr(begin, comma - begin);
        std::size_t start = 0;
        std::size_t end = part.size();
        while (start < end && std::isspace(static_cast<unsigned char>(part[start])) != 0)
        {
            ++start;
        }
        while (end > start && std::isspace(static_cast<unsigned char>(part[end - 1])) != 0)
        {
            --end;
        }
        if (start < end)
        {
            parts.emplace_back(part.substr(start, end - start));
        }
        if (comma == std::string_view::npos)
        {
            break;
        }
        begin = comma + 1;
    }
    return parts;
}

[[nodiscard]] std::expected<std::vector<std::size_t>, CommandOutput> ProjectColumns(
    const Options &options, const loglib::LogTable &table
)
{
    const auto &columns = table.Configuration().Configuration().columns;
    if (options.allColumns)
    {
        std::vector<std::size_t> indices(columns.size());
        for (std::size_t i = 0; i < columns.size(); ++i)
        {
            indices[i] = i;
        }
        return indices;
    }
    if (options.columns)
    {
        std::vector<std::size_t> indices;
        for (const std::string &name : SplitCsv(*options.columns))
        {
            const int index = FindColumnByName(columns, name);
            if (index < 0)
            {
                return std::unexpected(QueryFail(
                    ExitCode::Usage,
                    std::format(
                        "unresolvable column '{}'; available columns are {}.", name, AvailableColumnList(columns)
                    )
                ));
            }
            indices.push_back(static_cast<std::size_t>(index));
        }
        return indices;
    }
    return loglib::ColumnProjection(columns).Indices();
}

} // namespace

CommandOutput RunQuery(const Options &options, const RuntimeContext &runtime)
{
    CommandOutput out;
    if (!options.quiet && (options.config || options.saveSession || options.saveColumns))
    {
        out.err += FormatWarningLine("session configuration flags are not applied yet");
        out.err.push_back('\n');
    }

    const auto parseStarted = std::chrono::steady_clock::now();
    auto [loaded, loadSide] = LoadTable(options, runtime);
    out.err += loadSide.err;
    if (loadSide.exitCode != 0)
    {
        out.exitCode = loadSide.exitCode;
        return out;
    }
    std::vector<std::shared_ptr<loglib::DecompressingByteSource>> ownedDecompressors =
        std::move(loaded.ownedDecompressors);
    std::vector<std::string> parseErrors = std::move(loaded.parseErrors);
    loglib::LogTable table = std::move(loaded.table);
    const auto parseMs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - parseStarted).count()
    );
    if (options.verbose > 0)
    {
        out.err += FormatStageLine("parse", table.RowCount(), parseMs);
        out.err.push_back('\n');
    }

    auto filter = BuildRowFilter(options, table, runtime, "query");
    if (!filter)
    {
        out.err += filter.error().err;
        out.exitCode = filter.error().exitCode;
        return out;
    }

    const auto filterStarted = std::chrono::steady_clock::now();
    const auto compiled = loglib::CompileExpression(*filter, table.Configuration().Configuration().columns, &table);
    std::vector<std::size_t> rows = loglib::FilterAcceptedRows(table, compiled);
    const auto filterMs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - filterStarted).count()
    );
    if (options.verbose > 0)
    {
        out.err += FormatStageLine("filter", rows.size(), filterMs);
        out.err.push_back('\n');
    }

    if (options.sort)
    {
        std::string_view sortSpec = *options.sort;
        loglib::SortDirection direction = loglib::SortDirection::Ascending;
        const std::size_t colon = sortSpec.rfind(':');
        if (colon != std::string_view::npos)
        {
            const std::string_view dir = sortSpec.substr(colon + 1);
            if (EqualsIgnoreCase(dir, "asc"))
            {
                direction = loglib::SortDirection::Ascending;
                sortSpec = sortSpec.substr(0, colon);
            }
            else if (EqualsIgnoreCase(dir, "desc"))
            {
                direction = loglib::SortDirection::Descending;
                sortSpec = sortSpec.substr(0, colon);
            }
        }
        const auto &columns = table.Configuration().Configuration().columns;
        const int columnIndex = FindColumnByName(columns, sortSpec);
        if (columnIndex < 0)
        {
            return QueryFail(
                ExitCode::Usage,
                std::format(
                    "unresolvable sort column '{}'; available columns are {}.", sortSpec, AvailableColumnList(columns)
                )
            );
        }
        std::optional<loglib::EnumDictRank> rankStorage;
        const loglib::EnumDictRank *rank = nullptr;
        if (columns[static_cast<std::size_t>(columnIndex)].type == loglib::ColumnType::Enumeration)
        {
            const auto lookup = table.ResolveEnumColumn(static_cast<std::size_t>(columnIndex));
            if (lookup.dictionary != nullptr)
            {
                rankStorage.emplace(*lookup.dictionary);
                rank = &*rankStorage;
            }
        }
        const loglib::RowOrdering ordering(static_cast<std::size_t>(columnIndex), direction, rank);
        rows = ordering.Permute(table, std::span<const std::size_t>{rows});
    }

    if (options.last)
    {
        if (rows.size() > *options.last)
        {
            rows.erase(rows.begin(), rows.end() - static_cast<std::ptrdiff_t>(*options.last));
        }
    }
    else
    {
        std::size_t start = 0;
        if (options.offset)
        {
            start = static_cast<std::size_t>(std::min<std::uint64_t>(*options.offset, rows.size()));
        }
        if (start > 0)
        {
            rows.erase(rows.begin(), rows.begin() + static_cast<std::ptrdiff_t>(start));
        }
        if (options.limit && rows.size() > *options.limit)
        {
            rows.resize(static_cast<std::size_t>(*options.limit));
        }
    }

    if (!parseErrors.empty())
    {
        out.err += FormatErrorLine(
            {}, std::format("{} lines could not be parsed (use --verbose to list)", parseErrors.size())
        );
        out.err.push_back('\n');
        if (options.verbose > 0)
        {
            for (const std::string &error : parseErrors)
            {
                out.err += FormatWarningLine(error);
                out.err.push_back('\n');
            }
        }
    }

    if (options.count)
    {
        out.out = std::to_string(rows.size());
        out.out.push_back('\n');
        if (options.strict && !parseErrors.empty())
        {
            out.exitCode = static_cast<int>(ExitCode::Data);
            return out;
        }
        out.exitCode = rows.empty() && !options.allowEmpty ? static_cast<int>(ExitCode::Empty)
                                                           : static_cast<int>(ExitCode::Success);
        return out;
    }

    auto visible = ProjectColumns(options, table);
    if (!visible)
    {
        out.err += visible.error().err;
        out.exitCode = visible.error().exitCode;
        return out;
    }

    const auto exportFormat = ToExportFormat(options.outputFormat);
    if (!exportFormat)
    {
        return QueryFail(
            ExitCode::Usage,
            std::format("'--output-format' expects jsonl, csv, markdown, or snapshot, not '{}'.", options.outputFormat)
        );
    }

    if (*exportFormat != loglib::exports::ExportFormat::Csv &&
        *exportFormat != loglib::exports::ExportFormat::Markdown && options.noHeader)
    {
        out.err += FormatWarningLine("'--no-header' has no effect for this output format");
        out.err.push_back('\n');
    }

    loglib::exports::ExportPlan plan;
    plan.format = *exportFormat;
    plan.sourceRows = std::move(rows);
    plan.visibleColumns = std::move(*visible);
    plan.includeAllFieldsForJson = !options.jsonVisibleOnly;
    plan.includeHeaderRow = !options.noHeader;
    plan.table = &table;

    auto exporter = loglib::exports::MakeExporter(plan.format);
    if (!exporter)
    {
        return QueryFail(ExitCode::Usage, "unsupported export format.");
    }

    const bool empty = plan.sourceRows.empty();
    const bool toStdout = options.output == "-" || options.output.empty();
    if (!toStdout)
    {
        std::error_code ec;
        if (std::filesystem::exists(options.output, ec) && !options.force)
        {
            return QueryFail(
                ExitCode::Output, std::format("output '{}' already exists; pass --force to overwrite.", options.output)
            );
        }
        loglib::exports::FileSink sink(options.output);
        exporter->Run(plan.View(), sink, runtime.stop);
        sink.Finish();
    }
    else
    {
        // Write through StreamSink so a closed pipe surfaces as BrokenPipe
        // and large results are not buffered in memory. Execute must not
        // reprint stdout for query when `out` is empty.
        loglib::exports::StreamSink sink(stdout);
        exporter->Run(plan.View(), sink, runtime.stop);
        sink.Finish();
    }

    if (options.strict && !parseErrors.empty())
    {
        out.exitCode = static_cast<int>(ExitCode::Data);
        return out;
    }
    out.exitCode =
        empty && !options.allowEmpty ? static_cast<int>(ExitCode::Empty) : static_cast<int>(ExitCode::Success);
    return out;
}

} // namespace slp
