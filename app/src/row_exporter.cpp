#include "row_exporter.hpp"

#include <loglib/internal/normalized_json_row.hpp>
#include <loglib/key_index.hpp>
#include <loglib/line_source.hpp>
#include <loglib/log_data.hpp>
#include <loglib/log_line.hpp>
#include <loglib/log_value.hpp>

#include <fmt/format.h>

#include <cassert>
#include <cstddef>
#include <string>
#include <string_view>

namespace slv::exports
{

namespace
{

/// Poll cancellation every 256 rows.
constexpr size_t STOP_POLL_INTERVAL_ROWS = 4096;

/// Initial per-row output capacity.
constexpr size_t ROW_SCRATCH_RESERVE = 512;

/// Throw `ExportCancelled` when the user hit Cancel. Called
/// between row batches.
void PollStop(const loglib::StopToken &token)
{
    if (token.stop_requested())
    {
        throw ExportCancelled{};
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
        ProgressCallback progress,
        void *progressUserData
    ) override;
};

void JsonLinesExporter::Run(
    const RowSource &source,
    ExportSink &sink,
    const loglib::StopToken &stopToken,
    ProgressCallback progress,
    void *progressUserData
)
{
    assert(source.table != nullptr);
    const auto &table = *source.table;
    const auto &data = table.Data();
    const auto &lines = data.Lines();
    const auto &keys = data.Keys();

    std::string scratch;
    scratch.reserve(ROW_SCRATCH_RESERVE);

    const size_t total = source.sourceRows.size();
    for (size_t slot = 0; slot < total; ++slot)
    {
        if ((slot % STOP_POLL_INTERVAL_ROWS) == 0)
        {
            PollStop(stopToken);
        }
        const int sourceRow = source.sourceRows[slot];
        if (sourceRow < 0 || static_cast<size_t>(sourceRow) >= lines.size())
        {
            continue;
        }
        const auto &line = lines[static_cast<size_t>(sourceRow)];
        scratch.clear();
        loglib::internal::SerializeNormalizedJsonRow(line, keys, scratch);
        scratch.push_back('\n');
        sink.Write(scratch);

        if (progress != nullptr && ((slot + 1) % STOP_POLL_INTERVAL_ROWS) == 0)
        {
            progress(progressUserData, slot + 1, total);
        }
    }
    if (progress != nullptr)
    {
        progress(progressUserData, total, total);
    }
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
        ProgressCallback progress,
        void *progressUserData
    ) override;

private:
    /// Append one RFC 4180 cell. Prefix `=` and `@` values with `'`
    /// to prevent spreadsheet formula injection.
    static void AppendCsvCell(std::string &out, std::string_view cell);

    /// True iff @p cell starts with a spreadsheet formula / DDE
    /// prefix (`=` or `@`).
    [[nodiscard]] static bool IsFormulaTrigger(std::string_view cell) noexcept
    {
        if (cell.empty())
        {
            return false;
        }
        const char first = cell.front();
        return first == '=' || first == '@';
    }
};

void CsvExporter::AppendCsvCell(std::string &out, std::string_view cell)
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
    for (const char c : cell)
    {
        if (c == '"')
        {
            out.append("\"\"");
        }
        else
        {
            out.push_back(c);
        }
    }
    out.push_back('"');
}

void CsvExporter::Run(
    const RowSource &source,
    ExportSink &sink,
    const loglib::StopToken &stopToken,
    ProgressCallback progress,
    void *progressUserData
)
{
    assert(source.table != nullptr);
    const auto &table = *source.table;
    const auto &config = table.Configuration().Configuration();
    const auto &lines = table.Data().Lines();

    std::string scratch;
    scratch.reserve(ROW_SCRATCH_RESERVE);

    if (source.includeHeaderRow)
    {
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
                AppendCsvCell(scratch, config.columns[col].header);
            }
        }
        // Use LF consistently across export formats.
        scratch.push_back('\n');
        sink.Write(scratch);
        scratch.clear();
    }

    const size_t total = source.sourceRows.size();
    std::string cellBuffer;
    for (size_t slot = 0; slot < total; ++slot)
    {
        if ((slot % STOP_POLL_INTERVAL_ROWS) == 0)
        {
            PollStop(stopToken);
        }
        const int sourceRow = source.sourceRows[slot];
        // Skip negative / past-the-end indices so a mid-export
        // FIFO eviction on the GUI thread cannot hand us a
        // dropped row.
        if (sourceRow < 0 || static_cast<size_t>(sourceRow) >= lines.size())
        {
            continue;
        }

        scratch.clear();
        bool first = true;
        for (const size_t col : source.visibleColumns)
        {
            if (!first)
            {
                scratch.push_back(',');
            }
            first = false;
            cellBuffer.clear();
            const std::string_view formatted =
                table.GetValueOrFormatted(static_cast<size_t>(sourceRow), col, cellBuffer);
            AppendCsvCell(scratch, formatted);
        }
        scratch.push_back('\n');
        sink.Write(scratch);

        if (progress != nullptr && ((slot + 1) % STOP_POLL_INTERVAL_ROWS) == 0)
        {
            progress(progressUserData, slot + 1, total);
        }
    }
    if (progress != nullptr)
    {
        progress(progressUserData, total, total);
    }
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
        ProgressCallback progress,
        void *progressUserData
    ) override;
};

void SnapshotExporter::Run(
    const RowSource &source,
    ExportSink &sink,
    const loglib::StopToken &stopToken,
    ProgressCallback progress,
    void *progressUserData
)
{
    assert(source.table != nullptr);
    const auto &lines = source.table->Data().Lines();

    const size_t total = source.sourceRows.size();
    for (size_t slot = 0; slot < total; ++slot)
    {
        if ((slot % STOP_POLL_INTERVAL_ROWS) == 0)
        {
            PollStop(stopToken);
        }
        const int sourceRow = source.sourceRows[slot];
        if (sourceRow < 0 || static_cast<size_t>(sourceRow) >= lines.size())
        {
            continue;
        }
        const auto &line = lines[static_cast<size_t>(sourceRow)];
        const auto *lineSource = line.Source();
        if (lineSource == nullptr)
        {
            continue;
        }
        // Skip unavailable source rows, but let sink failures abort.
        std::string raw;
        try
        {
            raw = lineSource->RawLine(line.LineId());
        }
        catch (const std::exception &)
        {
            continue;
        }
        sink.Write(raw);
        sink.WriteChar('\n');

        if (progress != nullptr && ((slot + 1) % STOP_POLL_INTERVAL_ROWS) == 0)
        {
            progress(progressUserData, slot + 1, total);
        }
    }
    if (progress != nullptr)
    {
        progress(progressUserData, total, total);
    }
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
        ProgressCallback progress,
        void *progressUserData
    ) override;

private:
    /// Escape pipes/backslashes and collapse whitespace for a
    /// single-line Markdown table cell.
    static void AppendMarkdownCell(std::string &out, std::string_view cell);
};

void MarkdownExporter::AppendMarkdownCell(std::string &out, std::string_view cell)
{
    for (const char c : cell)
    {
        switch (c)
        {
        case '|':
            out.append("\\|");
            break;
        case '\\':
            out.append("\\\\");
            break;
        case '\n':
        case '\r':
        case '\t':
            out.push_back(' ');
            break;
        default:
            out.push_back(c);
            break;
        }
    }
}

void MarkdownExporter::Run(
    const RowSource &source,
    ExportSink &sink,
    const loglib::StopToken &stopToken,
    ProgressCallback progress,
    void *progressUserData
)
{
    assert(source.table != nullptr);
    const auto &table = *source.table;
    const auto &config = table.Configuration().Configuration();
    const auto &lines = table.Data().Lines();

    std::string scratch;
    scratch.reserve(ROW_SCRATCH_RESERVE);

    if (source.includeHeaderRow && !source.visibleColumns.empty())
    {
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
        scratch.clear();
    }

    const size_t total = source.sourceRows.size();
    std::string cellBuffer;
    for (size_t slot = 0; slot < total; ++slot)
    {
        if ((slot % STOP_POLL_INTERVAL_ROWS) == 0)
        {
            PollStop(stopToken);
        }
        const int sourceRow = source.sourceRows[slot];
        // Skip negative / past-the-end indices; matches JsonLines
        // / Snapshot so a mid-export FIFO eviction on the GUI
        // thread cannot hand us a dropped row.
        if (sourceRow < 0 || static_cast<size_t>(sourceRow) >= lines.size())
        {
            continue;
        }

        scratch.clear();
        scratch.push_back('|');
        for (const size_t col : source.visibleColumns)
        {
            scratch.push_back(' ');
            cellBuffer.clear();
            const std::string_view formatted =
                table.GetValueOrFormatted(static_cast<size_t>(sourceRow), col, cellBuffer);
            AppendMarkdownCell(scratch, formatted);
            scratch.append(" |");
        }
        scratch.push_back('\n');
        sink.Write(scratch);

        if (progress != nullptr && ((slot + 1) % STOP_POLL_INTERVAL_ROWS) == 0)
        {
            progress(progressUserData, slot + 1, total);
        }
    }
    if (progress != nullptr)
    {
        progress(progressUserData, total, total);
    }
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

} // namespace slv::exports
