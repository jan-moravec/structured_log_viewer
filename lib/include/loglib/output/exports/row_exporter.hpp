/**
 * @brief Writes table rows in several formats.
 *
 * A RowExporter takes the visible columns and the chosen rows of a table and writes them to an ExportSink as JSON
 * Lines, CSV, plain log lines or Markdown. It reports progress and stops when its stop token is triggered.
 *
 * This is part of the output module.
 */
#pragma once

#include "loglib/core/stop_token.hpp"
#include "loglib/output/exports/export_sink.hpp"
#include "loglib/schema/log_configuration.hpp"
#include "loglib/table/log_table.hpp"

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
 * @brief The formats rows can be exported in. The values are stored in saved settings, so new formats go at the end.
 */
enum class ExportFormat : int
{
    JsonLines = 0,
    Csv = 1,
    Snapshot = 2,
    Markdown = 3,
};

/** @brief The usual file extension of a format, without the dot: jsonl, csv, log or md. */
[[nodiscard]] const char *ExtensionFor(ExportFormat format) noexcept;

/**
 * @brief The English name of a format, such as JSON Lines. It is not translated, so an application translates its own
 * labels. Tests use it so that format names stay in step with ExtensionFor.
 */
[[nodiscard]] const char *LabelFor(ExportFormat format) noexcept;

/**
 * @brief A read-only description of the rows and columns of one export.
 *
 * It is a snapshot. The table, and the vectors that the spans point into, must stay alive for the whole export.
 * ExportPlan owns those vectors so that a worker thread gets everything it needs in one object.
 */
struct RowSource
{
    /** @brief The table to read cells from. A worker thread may read it as long as nothing writes to it. */
    const loglib::LogTable *table = nullptr;

    /** @brief The rows of the table to export, in display order. Filtering and sorting have already been applied. */
    std::span<const std::size_t> sourceRows;

    /**
     * @brief The columns to write for CSV and Markdown, as indices into LogConfiguration::columns, in display order.
     *
     * JSON Lines ignores them when includeAllFieldsForJson is set, and the snapshot format does not use them.
     */
    std::span<const std::size_t> visibleColumns;

    /**
     * @brief With true, JSON Lines writes every field of the row. Otherwise it writes only the visible columns. CSV,
     * Markdown and the snapshot format ignore it.
     */
    bool includeAllFieldsForJson = true;

    /** @brief With true, CSV and Markdown start with a header row. JSON Lines and the snapshot format ignore it. */
    bool includeHeaderRow = true;
};

/**
 * @brief Writes rows in one format. Make one for each export run.
 *
 * Run goes through the rows of the RowSource and writes the bytes to the sink in row order. Several threads turn the
 * rows into text, so the cells are read from more than one thread, but the sink and the progress callback are called by
 * one thread at a time. Run checks the stop token every few rows and throws OperationCancelled when a stop is
 * requested. Run does not call Finish. The caller does that after a successful run, so that an exception during the run
 * lets ~FileSink delete the temporary file.
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
     * @brief Writes the rows of the source to the sink.
     *
     * @param progress Called with the progress. May be empty.
     */
    virtual void Run(
        const RowSource &source,
        ExportSink &sink,
        const loglib::StopToken &stopToken,
        const RowProgressCallback &progress = {}
    ) = 0;
};

/** @brief Creates an exporter for a format. Returns null for a value that is not a format. */
[[nodiscard]] std::unique_ptr<RowExporter> MakeExporter(ExportFormat format);

/**
 * @brief Adds one field to a CSV line, as RFC 4180 describes.
 *
 * The field is put in quotes when it contains a comma, a quote or a line break, and quotes inside are doubled. A value
 * that starts with = or @ is quoted and gets a ' in front, so that spreadsheets do not run it as a formula.
 */
void AppendCsvField(std::string &out, std::string_view cell);

/**
 * @brief Everything one export needs, in one object that can be handed to a worker thread.
 *
 * It owns the vectors that the spans of RowSource point into, so the worker does not depend on the state of the thread
 * that started it.
 */
struct ExportPlan
{
    ExportFormat format = ExportFormat::JsonLines;

    /** @brief The rows of the table to export, in display order. */
    std::vector<std::size_t> sourceRows;

    /** @brief The columns to write, in display order. */
    std::vector<std::size_t> visibleColumns;

    bool includeAllFieldsForJson = true;
    bool includeHeaderRow = true;

    /**
     * @brief The table, which is borrowed. The caller makes sure it and its configuration stay alive while the worker
     * runs and that nothing changes them during the export.
     */
    const loglib::LogTable *table = nullptr;

    /**
     * @brief The path of the output file. FileSink writes to a temporary file next to it and renames it when the
     * export succeeds.
     */
    std::filesystem::path destination;

    /** @brief Returns a RowSource over the vectors of this plan. */
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
