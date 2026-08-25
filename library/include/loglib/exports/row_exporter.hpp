#pragma once

#include "loglib/exports/export_sink.hpp"
#include "loglib/log_configuration.hpp"
#include "loglib/log_table.hpp"
#include "loglib/stop_token.hpp"

#include <cstddef>
#include <exception>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace loglib::exports
{

/**
 * @brief Supported row-export formats.
 *
 * New values append at the end so persisted `QSettings` integers
 * keep pointing at the same format across upgrades.
 */
enum class ExportFormat : int
{
    JsonLines = 0,
    Csv = 1,
    Snapshot = 2,
    Markdown = 3,
};

/**
 * @brief Returns the preferred filename extension for @p format.
 *
 * The result has no leading dot (`"jsonl"`, `"csv"`, `"log"`, `"md"`).
 *
 * @param format Export format.
 * @return Extension without a leading dot.
 */
[[nodiscard]] const char *ExtensionFor(ExportFormat format) noexcept;

/**
 * @brief Returns an untranslated English name for @p format.
 *
 * GUI labels go through `tr(...)` in `app/`. Tests use this helper
 * so format names cannot drift from `ExtensionFor`.
 *
 * @param format Export format.
 * @return English label such as `"JSON Lines"`.
 */
[[nodiscard]] const char *LabelFor(ExportFormat format) noexcept;

/**
 * @brief Read-only view over one row-export run.
 *
 * The consumer treats this as a materialised snapshot: `table` and
 * the vectors behind the spans must outlive the export. Ownership
 * of those vectors lives on `ExportPlan` so an async worker gets a
 * self-contained bundle.
 */
struct RowSource
{
    /**
     * @brief Table to read cells from.
     *
     * Safe to read from a worker thread as long as no writer is
     * running (the GUI-thread snapshot pattern).
     */
    const loglib::LogTable *table = nullptr;

    /**
     * @brief `LogTable` row indices in display order.
     *
     * Filters and sort are already applied. The GUI converts
     * `int` model rows at the `ExportPlan` boundary.
     */
    std::span<const std::size_t> sourceRows;

    /**
     * @brief Columns to emit for CSV / Markdown, in display order.
     *
     * Indices into `LogConfiguration::columns`. Unused when JSON
     * Lines has `includeAllFieldsForJson` (every field on the row)
     * and unused by Snapshot (row-shape format).
     */
    std::span<const std::size_t> visibleColumns;

    /**
     * @brief When true, JSON Lines emits every field on the row.
     *
     * Otherwise it would honour `visibleColumns`. Ignored by CSV,
     * Markdown, and Snapshot.
     */
    bool includeAllFieldsForJson = true;

    /**
     * @brief When true, CSV / Markdown emit a header row.
     *
     * Ignored by JSON Lines and Snapshot.
     */
    bool includeHeaderRow = true;
};

/**
 * @brief Progress callback for a running export.
 *
 * `rowsWritten` is monotonically increasing; `totalRows` is the
 * size of `RowSource::sourceRows`. Cancellation flows through the
 * stop token, not the return value.
 */
using ProgressCallback = void (*)(void *userData, std::size_t rowsWritten, std::size_t totalRows);

/**
 * @brief Abstract row-oriented exporter. One instance per export run.
 *
 * `Run` walks `RowSource::sourceRows` in order and streams bytes to
 * the sink. It polls @p stopToken on a row batch cadence and throws
 * `ExportCancelled` on stop. `Run` does not call `sink.Finish()`;
 * the caller must call it on the success path so a mid-run throw
 * unlinks the temp file via `~FileSink`.
 */
class RowExporter
{
public:
    RowExporter() = default;
    virtual ~RowExporter() = default;

    RowExporter(const RowExporter &) = delete;
    RowExporter &operator=(const RowExporter &) = delete;
    RowExporter(RowExporter &&) = delete;
    RowExporter &operator=(RowExporter &&) = delete;

    /**
     * @brief Emits @p source through @p sink.
     *
     * @param source Snapshot of rows and columns to write.
     * @param sink Destination bytes.
     * @param stopToken Cancellation token polled between row batches.
     * @param progress Optional progress callback; may be null.
     * @param progressUserData Passed through to @p progress.
     */
    virtual void Run(
        const RowSource &source,
        ExportSink &sink,
        const loglib::StopToken &stopToken,
        ProgressCallback progress = nullptr,
        void *progressUserData = nullptr
    ) = 0;
};

/**
 * @brief Sentinel exception for user cancel.
 *
 * Deliberately not a `std::runtime_error` so plain error handlers
 * do not silently swallow it. Mirrors `DecompressionCancelled`.
 */
class ExportCancelled : public std::exception
{
public:
    [[nodiscard]] const char *what() const noexcept override
    {
        return "Export cancelled by user";
    }
};

/**
 * @brief Returns a fresh exporter for @p format.
 *
 * @param format Format to emit.
 * @return Owning pointer, or `nullptr` for an unknown enumerator.
 */
[[nodiscard]] std::unique_ptr<RowExporter> MakeExporter(ExportFormat format);

/**
 * @brief Self-contained bundle handed off to an async worker.
 *
 * Owns the snapshot vectors backing `RowSource`'s spans, so the
 * worker's lifetime is independent of GUI-thread state.
 */
struct ExportPlan
{
    ExportFormat format = ExportFormat::JsonLines;

    /** @brief `LogTable` row indices in display order. */
    std::vector<std::size_t> sourceRows;

    /** @brief Visible columns in display order. */
    std::vector<std::size_t> visibleColumns;

    bool includeAllFieldsForJson = true;
    bool includeHeaderRow = true;

    /**
     * @brief Borrowed table pointer.
     *
     * The caller guarantees `LogTable` (and its `LogConfiguration`)
     * outlive the worker; the GUI thread quiesces every writer for
     * the duration of the export.
     */
    const loglib::LogTable *table = nullptr;

    /**
     * @brief Destination path.
     *
     * `FileSink` writes to `<destination>.tmp` and atomically
     * renames on success.
     */
    std::filesystem::path destination;

    /** @brief Builds a `RowSource` view over this plan. */
    [[nodiscard]] RowSource View() const noexcept
    {
        return RowSource{
            .table = table,
            .sourceRows = std::span<const std::size_t>(sourceRows),
            .visibleColumns = std::span<const std::size_t>(visibleColumns),
            .includeAllFieldsForJson = includeAllFieldsForJson,
            .includeHeaderRow = includeHeaderRow,
        };
    }
};

} // namespace loglib::exports
