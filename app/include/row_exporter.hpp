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

/// Supported export formats. New formats must append at the end
/// so persisted `QSettings` values keep pointing to the same
/// format across upgrades.
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

/// Read-only view over one row-export run. The consumer treats
/// this as a materialised snapshot: `table` and the vectors behind
/// the spans must outlive the export. Ownership of the vectors
/// lives on `ExportPlan` so the async worker gets a self-contained
/// bundle.
struct RowSource
{
    /// Table to read cells from. Safe to read from a worker thread
    /// as long as no writer is running (guaranteed by the
    /// GUI-thread snapshot pattern).
    const loglib::LogTable *table = nullptr;

    /// Source-model row indices in display order (proxy row `P`
    /// maps to `sourceRows[P]`). Filters and sort are already
    /// applied.
    std::span<const int> sourceRows;

    /// Columns to emit for column-oriented formats (CSV / Markdown),
    /// in display order. Indices into `LogConfiguration::columns`.
    /// Unused when JSON Lines has `includeAllFieldsForJson=true`
    /// (which walks every (KeyId, Value) pair on the row) and
    /// unused by Snapshot (row-shape format).
    std::span<const size_t> visibleColumns;

    /// If true, JSON Lines emits every field on the row for
    /// round-trip fidelity; otherwise it uses `visibleColumns`.
    /// Ignored by CSV / Markdown / Snapshot.
    bool includeAllFieldsForJson = true;

    /// If true, CSV / Markdown emit a header row. Ignored by
    /// JSON Lines / Snapshot.
    bool includeHeaderRow = true;
};

/// Progress callback. `rowsWritten` is monotonically increasing;
/// `totalRows` is the size of `RowSource::sourceRows`. Cancellation
/// flows through the stop token, not the return value.
using ProgressCallback = void (*)(void *userData, size_t rowsWritten, size_t totalRows);

/// Abstract row-oriented exporter. One instance per export run.
///
/// Contract:
///   - `Run` walks `RowSource::sourceRows` in order and streams
///     bytes to @p sink.
///   - `Run` polls @p stopToken periodically (batched to keep
///     overhead down) and throws `ExportCancelled` on stop, so the
///     caller can distinguish user cancel from I/O error.
///   - `Run` does NOT call `sink.Finish()`; the caller must call
///     it on the success path. This keeps the atomic-rename side
///     effect behind a `try` boundary so a mid-run throw unlinks
///     the temp file via `~FileSink`.
class RowExporter
{
public:
    RowExporter() = default;
    virtual ~RowExporter() = default;

    RowExporter(const RowExporter &) = delete;
    RowExporter &operator=(const RowExporter &) = delete;
    RowExporter(RowExporter &&) = delete;
    RowExporter &operator=(RowExporter &&) = delete;

    /// Emit @p source through @p sink. @p progress may be null.
    /// Throws `ExportCancelled` on user cancel or
    /// `std::runtime_error` on I/O / serialization failure.
    virtual void
    Run(const RowSource &source,
        ExportSink &sink,
        const loglib::StopToken &stopToken,
        ProgressCallback progress = nullptr,
        void *progressUserData = nullptr) = 0;
};

/// Sentinel exception for user-cancel. Deliberately not a
/// `std::runtime_error` so that plain error handlers do not
/// silently swallow it. Mirrors `DecompressionCancelled`.
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

/// Self-contained bundle handed off to the async worker. Owns
/// the snapshot vectors backing `RowSource`'s spans, so the
/// worker's lifetime is independent of GUI-thread state.
struct ExportPlan
{
    ExportFormat format = ExportFormat::JsonLines;

    /// Proxy display order snapshotted at export-start time.
    std::vector<int> sourceRows;

    /// Visible columns in display order.
    std::vector<size_t> visibleColumns;

    bool includeAllFieldsForJson = true;
    bool includeHeaderRow = true;

    /// Borrowed table pointer. The caller guarantees `LogTable`
    /// (and its `LogConfiguration`) outlive the worker; the GUI
    /// thread quiesces every writer for the duration of the export.
    const loglib::LogTable *table = nullptr;

    /// Destination path. `FileSink` writes to `<destination>.tmp`
    /// and atomically renames on success.
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
