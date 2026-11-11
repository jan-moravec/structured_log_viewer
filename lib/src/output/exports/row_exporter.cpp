#include "loglib/output/exports/row_exporter.hpp"

#include <loglib/core/key_index.hpp>
#include <loglib/core/line_source.hpp>
#include <loglib/core/log_data.hpp>
#include <loglib/core/log_line.hpp>
#include <loglib/core/log_value.hpp>
#include <loglib/output/internal/normalized_json_row.hpp>
#include <loglib/output/internal/ordered_chunks.hpp>

#include <fmt/format.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace loglib::exports
{

namespace
{

/** The rows that one thread writes as a unit. Cancellation is checked and progress is reported once for each. */
constexpr size_t STOP_POLL_INTERVAL_ROWS = 4096;

/// Throw `OperationCancelled` when the user hit Cancel. Called
/// between row batches.
void PollStop(const loglib::StopToken &token)
{
    if (token.StopRequested())
    {
        throw OperationCancelled{"Export cancelled by user"};
    }
}

/// Calls @p writeRow for every source row that still exists, polling cancellation between batches and reporting
/// progress. Rows past the end of the table, for example after a live-tail eviction, are skipped.
template <typename WriteRow>
void ForEachSourceRow(
    const RowSource &source,
    size_t lineCount,
    const loglib::StopToken &stopToken,
    const RowProgressCallback &progress,
    WriteRow writeRow
)
{
    const size_t total = source.sourceRows.size();
    for (size_t slot = 0; slot < total; ++slot)
    {
        if ((slot % STOP_POLL_INTERVAL_ROWS) == 0)
        {
            PollStop(stopToken);
        }
        const std::size_t sourceRow = source.sourceRows[slot];
        if (sourceRow < lineCount)
        {
            writeRow(sourceRow);
        }
        if (progress && ((slot + 1) % STOP_POLL_INTERVAL_ROWS) == 0)
        {
            progress(slot + 1, total);
        }
    }
    if (progress)
    {
        progress(total, total);
    }
}

/**
 * Writes the text of every source row that still exists to the sink, in row order. The rows are cut into chunks, and
 * each thread turns a chunk into text with its own Worker, made by makeWorker. writeRow(worker, sourceRow, text)
 * appends one row to text. Rows past the end of the table, for example after a live-tail eviction, are skipped.
 * Cancellation is checked before each chunk and progress is reported after each one, in order and one call at a time.
 */
template <typename Worker, typename MakeWorker, typename WriteRow>
void ExportRows(
    const RowSource &source,
    size_t lineCount,
    ExportSink &sink,
    const loglib::StopToken &stopToken,
    const RowProgressCallback &progress,
    MakeWorker makeWorker,
    WriteRow writeRow
)
{
    const size_t total = source.sourceRows.size();
    loglib::internal::WriteInOrderedChunks<Worker>(
        total,
        STOP_POLL_INTERVAL_ROWS,
        makeWorker,
        [&] { PollStop(stopToken); },
        [&](Worker &worker, size_t first, size_t count, std::string &text) {
            for (size_t slot = first; slot < first + count; ++slot)
            {
                const size_t sourceRow = source.sourceRows[slot];
                if (sourceRow < lineCount)
                {
                    writeRow(worker, sourceRow, text);
                }
            }
        },
        [&](size_t first, size_t count, const std::string &text) {
            if (!text.empty())
            {
                sink.Write(text);
            }
            const size_t done = first + count;
            if (progress && (done % STOP_POLL_INTERVAL_ROWS) == 0)
            {
                progress(done, total);
            }
        }
    );
    if (progress)
    {
        progress(total, total);
    }
}

// -----------------------------------------------------------------
// JSON Lines
// -----------------------------------------------------------------

class JsonLinesExporter final : public RowExporter
{
public:
    void Run(
        const RowSource &source,
        ExportSink &sink,
        const loglib::StopToken &stopToken,
        const RowProgressCallback &progress
    ) override;
};

void JsonLinesExporter::Run(
    const RowSource &source, ExportSink &sink, const loglib::StopToken &stopToken, const RowProgressCallback &progress
)
{
    assert(source.table != nullptr);
    const auto &table = *source.table;
    const auto &data = table.Data();
    const auto &lines = data.Lines();

    ExportRows<loglib::internal::NormalizedJsonRowWriter>(
        source,
        lines.size(),
        sink,
        stopToken,
        progress,
        [&] { return loglib::internal::NormalizedJsonRowWriter(table); },
        [&](loglib::internal::NormalizedJsonRowWriter &writer, size_t sourceRow, std::string &text) {
            writer.Append(lines[sourceRow], text);
            text.push_back('\n');
        }
    );
}

// -----------------------------------------------------------------
// CSV (RFC 4180)
// -----------------------------------------------------------------

class CsvExporter final : public RowExporter
{
public:
    void Run(
        const RowSource &source,
        ExportSink &sink,
        const loglib::StopToken &stopToken,
        const RowProgressCallback &progress
    ) override;
};

/// True iff @p cell starts with a spreadsheet formula / DDE prefix (`=` or `@`).
[[nodiscard]] bool IsFormulaTrigger(std::string_view cell) noexcept
{
    return !cell.empty() && (cell.front() == '=' || cell.front() == '@');
}

} // namespace

void AppendCsvField(std::string &out, std::string_view cell)
{
    const bool formulaTrigger = IsFormulaTrigger(cell);
    const bool hasSpecial = cell.find_first_of(",\"\r\n") != std::string_view::npos;
    const bool needsQuote = hasSpecial || formulaTrigger;
    if (!needsQuote)
    {
        out.append(cell);
        return;
    }
    out.push_back('"');
    if (formulaTrigger)
    {
        // The leading apostrophe forces literal spreadsheet text.
        out.push_back('\'');
    }
    // Text between quotes goes out in one call per run.
    std::string_view rest = cell;
    for (std::size_t quote = rest.find('"'); quote != std::string_view::npos; quote = rest.find('"'))
    {
        out.append(rest.substr(0, quote + 1));
        out.push_back('"');
        rest.remove_prefix(quote + 1);
    }
    out.append(rest);
    out.push_back('"');
}

namespace
{

void CsvExporter::Run(
    const RowSource &source, ExportSink &sink, const loglib::StopToken &stopToken, const RowProgressCallback &progress
)
{
    assert(source.table != nullptr);
    const auto &table = *source.table;
    const auto &config = table.Configuration().Configuration();
    const auto &lines = table.Data().Lines();

    if (source.includeHeaderRow)
    {
        std::string scratch;
        bool first = true;
        for (const size_t col : source.visibleColumns)
        {
            if (!first)
            {
                scratch.push_back(',');
            }
            first = false;
            if (col < config.columns.size())
            {
                AppendCsvField(scratch, config.columns[col].header);
            }
        }
        // Use LF consistently across export formats.
        scratch.push_back('\n');
        sink.Write(scratch);
    }

    ExportRows<std::string>(
        source,
        lines.size(),
        sink,
        stopToken,
        progress,
        [] { return std::string{}; },
        [&](std::string &cellBuffer, size_t sourceRow, std::string &text) {
            bool first = true;
            for (const size_t col : source.visibleColumns)
            {
                if (!first)
                {
                    text.push_back(',');
                }
                first = false;
                cellBuffer.clear();
                const std::string_view formatted = table.GetValueOrFormatted(sourceRow, col, cellBuffer);
                AppendCsvField(text, formatted);
            }
            text.push_back('\n');
        }
    );
}

// -----------------------------------------------------------------
// Source snapshot (raw bytes per row)
// -----------------------------------------------------------------

class SnapshotExporter final : public RowExporter
{
public:
    void Run(
        const RowSource &source,
        ExportSink &sink,
        const loglib::StopToken &stopToken,
        const RowProgressCallback &progress
    ) override;
};

void SnapshotExporter::Run(
    const RowSource &source, ExportSink &sink, const loglib::StopToken &stopToken, const RowProgressCallback &progress
)
{
    assert(source.table != nullptr);
    const auto &lines = source.table->Data().Lines();

    ForEachSourceRow(source, lines.size(), stopToken, progress, [&](size_t sourceRow) {
        const auto &line = lines[sourceRow];
        const auto *lineSource = line.Source();
        if (lineSource == nullptr)
        {
            return;
        }
        // Skip unavailable source rows, but let sink failures abort.
        std::string raw;
        try
        {
            raw = lineSource->RawLine(line.LineId());
        }
        catch (const std::exception &)
        {
            return;
        }
        sink.Write(raw);
        sink.WriteChar('\n');
    });
}

// -----------------------------------------------------------------
// Markdown table
// -----------------------------------------------------------------

class MarkdownExporter final : public RowExporter
{
public:
    void Run(
        const RowSource &source,
        ExportSink &sink,
        const loglib::StopToken &stopToken,
        const RowProgressCallback &progress
    ) override;

private:
    /// Escape pipes/backslashes and collapse whitespace for a
    /// single-line Markdown table cell.
    static void AppendMarkdownCell(std::string &out, std::string_view cell);
};

void MarkdownExporter::AppendMarkdownCell(std::string &out, std::string_view cell)
{
    // Text without a special character goes out in one call per run.
    std::size_t runStart = 0;
    for (std::size_t i = 0; i < cell.size(); ++i)
    {
        const char c = cell[i];
        if (c != '|' && c != '\\' && c != '\n' && c != '\r' && c != '\t')
        {
            continue;
        }
        out.append(cell.data() + runStart, i - runStart);
        runStart = i + 1;
        if (c == '|')
        {
            out.append("\\|");
        }
        else if (c == '\\')
        {
            out.append("\\\\");
        }
        else
        {
            out.push_back(' ');
        }
    }
    out.append(cell.data() + runStart, cell.size() - runStart);
}

void MarkdownExporter::Run(
    const RowSource &source, ExportSink &sink, const loglib::StopToken &stopToken, const RowProgressCallback &progress
)
{
    assert(source.table != nullptr);
    const auto &table = *source.table;
    const auto &config = table.Configuration().Configuration();
    const auto &lines = table.Data().Lines();

    if (source.includeHeaderRow && !source.visibleColumns.empty())
    {
        std::string scratch;
        // Header row.
        scratch.push_back('|');
        for (const size_t col : source.visibleColumns)
        {
            scratch.push_back(' ');
            if (col < config.columns.size())
            {
                AppendMarkdownCell(scratch, config.columns[col].header);
            }
            scratch.append(" |");
        }
        scratch.push_back('\n');
        // Separator row.
        scratch.push_back('|');
        for (size_t i = 0; i < source.visibleColumns.size(); ++i)
        {
            scratch.append(" --- |");
        }
        scratch.push_back('\n');
        sink.Write(scratch);
    }

    ExportRows<std::string>(
        source,
        lines.size(),
        sink,
        stopToken,
        progress,
        [] { return std::string{}; },
        [&](std::string &cellBuffer, size_t sourceRow, std::string &text) {
            text.push_back('|');
            for (const size_t col : source.visibleColumns)
            {
                text.push_back(' ');
                cellBuffer.clear();
                const std::string_view formatted = table.GetValueOrFormatted(sourceRow, col, cellBuffer);
                AppendMarkdownCell(text, formatted);
                text.append(" |");
            }
            text.push_back('\n');
        }
    );
}

} // namespace

// Compiler warnings catch missing enum cases; assert handles bad values.
const char *ExtensionFor(ExportFormat format) noexcept
{
    switch (format)
    {
    case ExportFormat::JsonLines:
        return "jsonl";
    case ExportFormat::Csv:
        return "csv";
    case ExportFormat::Snapshot:
        return "log";
    case ExportFormat::Markdown:
        return "md";
    }
    assert(false && "unknown ExportFormat");
    return "jsonl";
}

const char *LabelFor(ExportFormat format) noexcept
{
    switch (format)
    {
    case ExportFormat::JsonLines:
        return "JSON Lines";
    case ExportFormat::Csv:
        return "CSV";
    case ExportFormat::Snapshot:
        return "Source snapshot";
    case ExportFormat::Markdown:
        return "Markdown table";
    }
    assert(false && "unknown ExportFormat");
    return "JSON Lines";
}

std::unique_ptr<RowExporter> MakeExporter(ExportFormat format)
{
    switch (format)
    {
    case ExportFormat::JsonLines:
        return std::make_unique<JsonLinesExporter>();
    case ExportFormat::Csv:
        return std::make_unique<CsvExporter>();
    case ExportFormat::Snapshot:
        return std::make_unique<SnapshotExporter>();
    case ExportFormat::Markdown:
        return std::make_unique<MarkdownExporter>();
    }
    assert(false && "unknown ExportFormat");
    return nullptr;
}

} // namespace loglib::exports
