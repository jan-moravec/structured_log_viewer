#pragma once

#include "export_sink.hpp"

#include <loglib/log_configuration.hpp>
#include <loglib/log_table.hpp>
#include <loglib/stop_token.hpp>

#include <cstddef>
#include <exception>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace slv::exports
{

/// Supported export formats. New formats append at the end so
/// persisted user preferences (last-used format via QSettings)
/// stay stable.
enum class ExportFormat : int
{
    JsonLines = 0,
    Csv = 1,
    Snapshot = 2,
    Markdown = 3,
};

/// Preferred filename extension for @p format (without leading dot).
[[nodiscard]] const char *ExtensionFor(ExportFormat format) noexcept;

/// Human-readable label for @p format (for the dialog dropdown and
/// the QFileDialog filter string).
[[nodiscard]] const char *LabelFor(ExportFormat format) noexcept;

/// Read-only view over one row-export session. The consumer thread
/// treats this as a materialised snapshot: `table` and the vectors
/// referenced by the spans must outlive the export.
///
/// Ownership of the underlying vectors lives on `ExportPlan` (see
/// below) so the async worker gets a self-contained bundle.
struct RowSource
{
    /// The `LogTable` to read cell values from. Reads are `const`
    /// and safe from a background thread as long as no writer is
    /// running (guaranteed by the GUI-thread snapshot pattern).
    const loglib::LogTable *table = nullptr;

    /// Source-model row indices in display order (i.e. proxy row
    /// `P` maps to `sourceRows[P]`). Filters and sort are already
    /// applied.
    std::span<const int> sourceRows;

    /// Column indices (into `table->Configuration().Configuration().columns`)
    /// to emit for the column-oriented formats (CSV / Markdown /
    /// JSON Lines-visible-only). Values are the display order, not
    /// necessarily contiguous. For JSON Lines with the "include
    /// all fields" toggle, the exporter walks the row's actual
    /// (KeyId, Value) pairs and this vector is unused.
    std::span<const size_t> visibleColumns;

    /// If true, JSON Lines emits every field on the row (round-trip
    /// fidelity) rather than only `visibleColumns`. CSV / Markdown
    /// ignore this flag.
    bool includeAllFieldsForJson = true;

    /// If true, CSV / Markdown emit a header row before the data.
    /// JSON Lines / Snapshot ignore this flag.
    bool includeHeaderRow = true;
};

/// Progress callback signature. `rowsWritten` is monotonically
/// increasing; `totalRows` is the size of `RowSource::sourceRows`.
/// Return value is ignored (cancellation flows through the stop
/// token, not the return value).
using ProgressCallback = void (*)(void *userData, size_t rowsWritten, size_t totalRows);

/// Abstract row-oriented exporter. One instance per export run.
///
/// Contract:
///   - `Run` walks `RowSource::sourceRows` in order and streams
///     bytes to @p sink.
///   - `Run` polls @p stopToken between rows (at most every N rows
///     to keep overhead down). On stop-requested it throws
///     `ExportCancelled` so the caller can distinguish user cancel
///     from I/O error.
///   - `Run` does NOT call `sink.Finish()`; that is the caller's
///     responsibility so a `try / catch` boundary can drop the
///     sink before the atomic-rename side effect.
class RowExporter
{
public:
    RowExporter() = default;
    virtual ~RowExporter() = default;

    RowExporter(const RowExporter &) = delete;
    RowExporter &operator=(const RowExporter &) = delete;
    RowExporter(RowExporter &&) = delete;
    RowExporter &operator=(RowExporter &&) = delete;

    /// Emit @p source through @p sink. Progress callback may be
    /// null. Throws `ExportCancelled` on user cancel, or
    /// `std::runtime_error` on I/O / serialization failure.
    virtual void
    Run(const RowSource &source,
        ExportSink &sink,
        const loglib::StopToken &stopToken,
        ProgressCallback progress = nullptr,
        void *progressUserData = nullptr) = 0;
};

/// Sentinel exception distinguishing user-cancel from other
/// std::exceptions (matches the `DecompressionCancelled` idiom in
/// `loglib::internal`). Not a `std::runtime_error` so plain error
/// handlers do not silently swallow it.
class ExportCancelled : public std::exception
{
public:
    [[nodiscard]] const char *what() const noexcept override
    {
        return "Export cancelled by user";
    }
};

/// Factory: return a fresh exporter for @p format.
[[nodiscard]] std::unique_ptr<RowExporter> MakeExporter(ExportFormat format);

/// Self-contained bundle handed off to the async worker. Owns the
/// snapshot vectors that back `RowSource`'s spans so the worker's
/// lifetime is independent of the GUI thread's state.
struct ExportPlan
{
    ExportFormat format = ExportFormat::JsonLines;

    /// Snapshot of proxy display order at export-start time.
    std::vector<int> sourceRows;

    /// Snapshot of visible columns in display order.
    std::vector<size_t> visibleColumns;

    bool includeAllFieldsForJson = true;
    bool includeHeaderRow = true;

    /// The table (borrowed pointer) plus a stable ref to the
    /// configuration used for column headers / print formats. The
    /// caller guarantees these outlive the worker: `LogTable` and
    /// `LogConfiguration` are stable across the export because the
    /// GUI thread has quiesced its writers.
    const loglib::LogTable *table = nullptr;

    /// Destination path. The `FileSink` prepends `.tmp` and
    /// renames on success.
    std::filesystem::path destination;

    /// Build a `RowSource` view over the plan.
    [[nodiscard]] RowSource View() const noexcept
    {
        return RowSource{
            .table = table,
            .sourceRows = std::span<const int>(sourceRows),
            .visibleColumns = std::span<const size_t>(visibleColumns),
            .includeAllFieldsForJson = includeAllFieldsForJson,
            .includeHeaderRow = includeHeaderRow,
        };
    }
};

} // namespace slv::exports
