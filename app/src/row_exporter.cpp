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

#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
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
///
/// The caller is expected to have already opened the JSON string
/// (`"`) before this call and to close it (`"`) after. The failure
/// fallback stays inside those quotes and emits the raw microsecond
/// count as a decimal string -- valid JSON, still human-inspectable,
/// but no longer ISO-parseable. A downstream reader that requires
/// an ISO string should treat any purely-numeric content between the
/// quotes as a diagnostic value rather than an instant.
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
        // `date::format` throws on far-future / far-past values past
        // its internal tables. Emit the raw microsecond count so the
        // row is still emitted (rather than aborting the export);
        // the JSON stays valid because we're already inside a `"…"`
        // opened by the caller.
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
                    // shortest round-trip). `{}` uses the shortest
                    // form that round-trips.
                    //
                    // Type-stability post-fix: whole-valued doubles
                    // (`1.0`, `-3.0`, ...) format as `1` / `-3` under
                    // default fmt shortest rules, which is valid JSON
                    // but re-parses as an integer -- annoying when a
                    // downstream reader keys behaviour off `typeof`
                    // (Python's `json`, JS `Number.isInteger`, jq).
                    // Detect the case and append `.0` so a Floating
                    // column always survives round-trip as a
                    // fractional literal.
                    const std::size_t before = out.size();
                    fmt::format_to(std::back_inserter(out), "{}", arg);
                    const std::string_view emitted(out.data() + before, out.size() - before);
                    if (emitted.find_first_of(".eE") == std::string_view::npos)
                    {
                        out.append(".0");
                    }
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
    ///
    /// **CSV formula-injection defense**: cells whose first byte is
    /// `=` or `@` are additionally prefixed with `'` and force-
    /// quoted. Excel / Google Sheets / LibreOffice treat these as
    /// formula / DDE prefixes and will *evaluate* the cell contents
    /// when the CSV is opened — a well-known vector (OWASP CSV
    /// Injection) that turns a shared log dump into arbitrary code
    /// execution on the recipient's machine. The `'` sentinel makes
    /// spreadsheets render the cell as literal text; plain text
    /// readers see `'=...` inside the quotes and can strip the
    /// sentinel if they need the raw value. Leading `+` / `-` are
    /// deliberately NOT rewritten because they also start every
    /// negative / signed number in the log and mangling those would
    /// break arithmetic in the vast majority of real spreadsheets.
    static void AppendCsvCell(std::string &out, std::string_view cell);

    /// True iff @p cell begins with a character a spreadsheet reader
    /// would interpret as a formula / DDE prefix.
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
        // Neutralise formula evaluation. `'` inside a quoted CSV cell
        // is not itself a special character (no doubling required)
        // and every spreadsheet treats the following content as
        // literal text.
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
        catch (const std::exception &) // NOLINT(bugprone-empty-catch)
        {
            // `RawLine` can throw `std::out_of_range` when a live-tail
            // FIFO has evicted the line, `std::runtime_error` when the
            // backing source (mmapped file, network stream) has gone
            // away since the plan was snapshotted, and other codec-
            // specific errors on partially-decoded compressed inputs.
            // The snapshot format is deliberately best-effort per row
            // -- swallow-and-continue matches the "one row per output
            // line" contract; the alternative (abort the whole export)
            // is worse UX and leaves nothing on disk.
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

} // namespace slv::exports
