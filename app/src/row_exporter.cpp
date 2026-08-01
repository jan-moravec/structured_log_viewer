#include "row_exporter.hpp"

#include <loglib/enum_dictionary.hpp>
#include <loglib/internal/compact_log_value.hpp>
#include <loglib/key_index.hpp>
#include <loglib/line_source.hpp>
#include <loglib/log_data.hpp>
#include <loglib/log_line.hpp>
#include <loglib/log_processing.hpp>
#include <loglib/log_value.hpp>

#include <date/date.h>
#include <date/tz.h>
#include <fmt/format.h>

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

namespace slv::exports
{

namespace
{

/// Poll the stop token every N rows. Keeps the check off the
/// per-cell fast path while capping cancel latency to a few ms
/// even on million-row exports.
constexpr size_t STOP_POLL_INTERVAL_ROWS = 4096;

/// Small write batching: exporters build a per-row `std::string`
/// scratch and flush it to the sink at the end of the row. Keeps
/// `fwrite` calls large without holding the whole export in memory.
constexpr size_t ROW_SCRATCH_RESERVE = 512;

/// Append a JSON-escaped copy of @p input to @p out (RFC 8259 §7).
/// Non-string bytes above 0x1F pass through verbatim (UTF-8 safe).
/// Control bytes below 0x20 that don't have a short escape use
/// `\u00XX`.
void AppendJsonEscaped(std::string &out, std::string_view input)
{
    out.reserve(out.size() + input.size() + 2);
    for (size_t i = 0; i < input.size(); ++i)
    {
        const auto ch = static_cast<unsigned char>(input[i]);
        switch (ch)
        {
        case '"':
            out.append("\\\"");
            break;
        case '\\':
            out.append("\\\\");
            break;
        case '\b':
            out.append("\\b");
            break;
        case '\f':
            out.append("\\f");
            break;
        case '\n':
            out.append("\\n");
            break;
        case '\r':
            out.append("\\r");
            break;
        case '\t':
            out.append("\\t");
            break;
        default:
            if (ch < 0x20)
            {
                fmt::format_to(std::back_inserter(out), "\\u{:04x}", static_cast<unsigned>(ch));
            }
            else
            {
                out.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
}

/// Serialise a `TimeStamp` as ISO 8601 with microsecond precision
/// in UTC. `date::format` with `%FT%T` on a UTC zoned_time already
/// includes fractional seconds derived from the underlying
/// precision (microseconds), and the trailing `Z` is appended
/// literally so the output is unambiguously UTC.
void AppendIsoTimestamp(std::string &out, loglib::TimeStamp ts)
{
    try
    {
        const date::sys_time<std::chrono::microseconds> sysTime{ts.time_since_epoch()};
        out.append(date::format("%FT%T", sysTime));
        out.push_back('Z');
    }
    catch (const std::exception &)
    {
        // date::format can throw on far-future values past its
        // internal tables. Fall back to a raw microsecond count so
        // the row is still emitted, just not ISO.
        fmt::format_to(std::back_inserter(out), "{}", ts.time_since_epoch().count());
    }
}

/// Serialise a `LogValue` as a JSON value (typed).
///
/// Rules:
///   - `std::monostate` -> `null` (caller should have skipped, but
///     defensive).
///   - `bool` -> `true` / `false`.
///   - `int64` / `uint64` -> JSON integer.
///   - `double` -> JSON number; NaN / Inf serialise as `null`
///     (JSON has no representation).
///   - `string_view` / `string` -> JSON-escaped string.
///   - `TimeStamp` -> ISO 8601 UTC string.
void AppendLogValueAsJson(std::string &out, const loglib::LogValue &value)
{
    std::visit(
        [&out]<class T>(const T &arg) {
            if constexpr (std::is_same_v<T, std::monostate>)
            {
                out.append("null");
            }
            else if constexpr (std::is_same_v<T, bool>)
            {
                out.append(arg ? "true" : "false");
            }
            else if constexpr (std::is_same_v<T, std::int64_t> || std::is_same_v<T, std::uint64_t>)
            {
                fmt::format_to(std::back_inserter(out), "{}", arg);
            }
            else if constexpr (std::is_same_v<T, double>)
            {
                if (!std::isfinite(arg))
                {
                    out.append("null");
                }
                else
                {
                    // `fmt`'s default double formatting matches JSON
                    // number grammar (no locale, no thousands sep,
                    // shortest round-trip). We rely on `{:g}` giving
                    // enough digits; `fmt` uses the shortest form
                    // that round-trips by default.
                    fmt::format_to(std::back_inserter(out), "{}", arg);
                }
            }
            else if constexpr (std::is_same_v<T, std::string_view>)
            {
                out.push_back('"');
                AppendJsonEscaped(out, arg);
                out.push_back('"');
            }
            else if constexpr (std::is_same_v<T, std::string>)
            {
                out.push_back('"');
                AppendJsonEscaped(out, std::string_view(arg));
                out.push_back('"');
            }
            else if constexpr (std::is_same_v<T, loglib::TimeStamp>)
            {
                out.push_back('"');
                AppendIsoTimestamp(out, arg);
                out.push_back('"');
            }
            else
            {
                static_assert(std::is_same_v<T, void>, "non-exhaustive AppendLogValueAsJson");
            }
        },
        value
    );
}

/// Poll the stop token, throwing `ExportCancelled` if the user hit
/// Cancel. Called between rows.
void PollStop(const loglib::StopToken &token)
{
    if (token.stop_requested())
    {
        throw ExportCancelled{};
    }
}

/// Materialise every present field on @p line as `(key, LogValue)`
/// pairs. Skips monostate slots (a `LogLine` should not carry them
/// in practice, but be defensive).
std::vector<std::pair<std::string_view, loglib::LogValue>>
MaterialiseRow(const loglib::LogLine &line, const loglib::KeyIndex &keys)
{
    const auto compact = line.CompactValues();
    std::vector<std::pair<std::string_view, loglib::LogValue>> out;
    out.reserve(compact.size());
    for (const auto &[keyId, slot] : compact)
    {
        if (slot.tag == loglib::internal::CompactTag::Monostate)
        {
            continue;
        }
        loglib::LogValue value = slot.Materialise(line.Source(), line.LineId(), keyId);
        if (std::holds_alternative<std::monostate>(value))
        {
            continue;
        }
        out.emplace_back(keys.KeyOf(keyId), std::move(value));
    }
    return out;
}

// -----------------------------------------------------------------
// JSON Lines
// -----------------------------------------------------------------

class JsonLinesExporter final : public RowExporter
{
public:
    void
    Run(const RowSource &source,
        ExportSink &sink,
        const loglib::StopToken &stopToken,
        ProgressCallback progress,
        void *progressUserData) override;
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
        const auto row = MaterialiseRow(line, keys);

        scratch.clear();
        scratch.push_back('{');
        bool first = true;
        for (const auto &[key, value] : row)
        {
            if (!first)
            {
                scratch.push_back(',');
            }
            first = false;
            scratch.push_back('"');
            AppendJsonEscaped(scratch, key);
            scratch.append("\":");
            AppendLogValueAsJson(scratch, value);
        }
        scratch.append("}\n");
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
    void
    Run(const RowSource &source,
        ExportSink &sink,
        const loglib::StopToken &stopToken,
        ProgressCallback progress,
        void *progressUserData) override;

private:
    /// Append @p cell to @p out, quoted per RFC 4180 iff it
    /// contains `,`, `"`, `\r`, or `\n`.
    static void AppendCsvCell(std::string &out, std::string_view cell);
};

void CsvExporter::AppendCsvCell(std::string &out, std::string_view cell)
{
    const bool needsQuote = cell.find_first_of(",\"\r\n") != std::string_view::npos;
    if (!needsQuote)
    {
        out.append(cell);
        return;
    }
    out.push_back('"');
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
        // RFC 4180 line ending is CRLF; also accepted by every
        // CSV reader we care about. Use LF-only for cross-platform
        // consistency with the rest of the exports (Excel accepts
        // both).
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
        // Bounds match JsonLines / Snapshot: skip both negative and
        // past-the-end indices so a mid-export FIFO eviction on the
        // GUI thread doesn't hand us a row that has been dropped.
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
    void
    Run(const RowSource &source,
        ExportSink &sink,
        const loglib::StopToken &stopToken,
        ProgressCallback progress,
        void *progressUserData) override;
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
        try
        {
            const std::string raw = lineSource->RawLine(line.LineId());
            sink.Write(raw);
            sink.WriteChar('\n');
        }
        catch (const std::out_of_range &) // NOLINT(bugprone-empty-catch)
        {
            // Line has been evicted from a live-tail source. Skip;
            // the snapshot is best-effort for the surviving rows.
        }

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
    void
    Run(const RowSource &source,
        ExportSink &sink,
        const loglib::StopToken &stopToken,
        ProgressCallback progress,
        void *progressUserData) override;

private:
    /// Append @p cell to @p out with Markdown-table-cell escaping:
    ///   - `|` -> `\|` (pipe would break the row).
    ///   - `\r`, `\n`, `\t` -> single space (Markdown table cells
    ///     cannot span lines).
    ///   - `\\` -> `\\\\` (so a literal backslash before a special
    ///     char is not misread as an escape).
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
        // Bounds match JsonLines / Snapshot: skip both negative and
        // past-the-end indices so a mid-export FIFO eviction on the
        // GUI thread doesn't hand us a row that has been dropped.
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
    return "txt";
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
    return "Unknown";
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
    return nullptr;
}

namespace
{

// Free function so its address is stable and the lambda-less capture
// keeps the code path allocation-free. `userData` is an atomic<size_t>*
// whose lifetime the caller owns.
void AtomicProgressAdapter(void *userData, size_t rowsWritten, size_t /*totalRows*/)
{
    auto *counter = static_cast<std::atomic<size_t> *>(userData);
    if (counter != nullptr)
    {
        counter->store(rowsWritten, std::memory_order_relaxed);
    }
}

} // namespace

void RunExport(
    const ExportPlan &plan,
    ExportSink &sink,
    const loglib::StopToken &stopToken,
    std::atomic<size_t> *rowsWritten
)
{
    auto exporter = MakeExporter(plan.format);
    if (exporter == nullptr)
    {
        throw std::runtime_error("Unsupported export format");
    }
    exporter->Run(plan.View(), sink, stopToken, &AtomicProgressAdapter, rowsWritten);
    sink.Finish();
    if (rowsWritten != nullptr)
    {
        rowsWritten->store(plan.sourceRows.size(), std::memory_order_relaxed);
    }
}

} // namespace slv::exports
