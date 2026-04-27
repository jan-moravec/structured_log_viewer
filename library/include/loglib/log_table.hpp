#pragma once

#include "key_index.hpp"
#include "log_configuration.hpp"
#include "log_data.hpp"
#include "log_file.hpp"
#include "streaming_log_sink.hpp"

#include <date/date.h>
#include <date/tz.h>
#include <fmt/format.h>

#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace loglib
{

class LogTable
{
public:
    LogTable() = default;

    /// Constructs a `LogTable` from existing data and configuration.
    LogTable(LogData data, LogConfigurationManager configuration);

    LogTable(LogTable &) = delete;
    LogTable &operator=(const LogTable &) = delete;

    LogTable(LogTable &&) = default;
    LogTable &operator=(LogTable &&) = default;

    /// Replaces the table's data with a freshly-merged @p data.
    void Update(LogData &&data);

    /**
     * @brief Initialises the table for an upcoming streaming parse.
     *
     * Installs @p file as the data source so subsequent `AppendBatch` calls
     * can splice batches in without owning a separate `LogData`, snapshots
     * the time-column KeyId set against the current configuration, and
     * resets `LastBackfillRange()` to `nullopt`. `Configuration()` is *not*
     * mutated; callers must lock the configuration UI between this call and
     * the matching streaming-finished signal.
     *
     * @param file Optional already-opened source file. Pass `nullptr` to
     *             keep the table file-less (e.g. for hand-crafted fixtures).
     */
    void BeginStreaming(std::unique_ptr<LogFile> file);

    /**
     * @brief Appends one streaming batch of pre-parsed lines.
     *
     * Behaviour:
     *   - Resets `LastBackfillRange()` to `nullopt` at entry.
     *   - If `batch.newKeys` is non-empty, extends the configuration's
     *     column list (always at the end) so any unmapped key appears as
     *     either a freshly-promoted `Type::time` column (when the auto-
     *     promotion heuristic matches) or a default `Type::any` column.
     *   - Splices `batch.lines` and `batch.localLineOffsets` into the owned
     *     `LogData` via `LogData::AppendBatch`.
     *   - For each `Type::time` column whose KeyId set is *not* a subset
     *     of the `BeginStreaming` snapshot, runs `BackfillTimestampColumn`
     *     over **all** rows so the new column has parsed timestamps
     *     everywhere. Records the affected column range in
     *     `mLastBackfillRange` so the model can issue a `dataChanged`.
     *
     * The function is intentionally `void`: all column / row count deltas
     * are observed via `RowCount()`/`ColumnCount()`.
     */
    void AppendBatch(StreamedBatch batch);

    /// Inclusive `[firstColumn, lastColumn]` range of columns that the most
    /// recent `AppendBatch` back-filled timestamps for, or `std::nullopt`
    /// if none. Reset on entry to every `AppendBatch`.
    const std::optional<std::pair<size_t, size_t>> &LastBackfillRange() const;

    /// Header text for the specified column.
    std::string GetHeader(size_t column) const;

    /// Total number of columns.
    size_t ColumnCount() const;

    /// Raw log value at @p row, @p column.
    LogValue GetValue(size_t row, size_t column) const;

    /// Formatted string representation of the value at @p row, @p column.
    std::string GetFormattedValue(size_t row, size_t column) const;

    /// Total number of rows.
    size_t RowCount() const;

    const LogData &Data() const;
    LogData &Data();

    const LogConfigurationManager &Configuration() const;
    LogConfigurationManager &Configuration();

private:
    static std::string FormatLogValue(const std::string &format, const LogValue &value);

    /**
     * @brief Rebuilds the column → KeyId cache from the current configuration.
     *
     * One inner vector per configured column listing the KeyIds of the
     * configuration's `column.keys`, resolved through `LogData::Keys()`.
     * Unknown keys become `kInvalidKeyId`. Called whenever data or
     * configuration changes wholesale (`Update`, ctor, `BeginStreaming`).
     */
    void RefreshColumnKeyIds();

    /**
     * @brief Incrementally patches the column → KeyId cache for columns
     *        that reference any key in @p newKeys.
     *
     * Used by `AppendBatch` whenever the streaming pipeline reports new
     * keys — the only situation where the `mColumnKeyIds` cache can have
     * gone stale (a previously unknown key may now resolve). Columns with
     * no overlap with @p newKeys keep their cached entries untouched: the
     * canonical KeyIndex is monotonic so resolved keys stay resolved.
     */
    void RefreshColumnKeyIdsForKeys(const std::vector<std::string> &newKeys);

    /// Shared body of `Update` and the snapshot captured by `BeginStreaming`.
    void RefreshSnapshotTimeKeys();

    LogData mData;
    LogConfigurationManager mConfiguration;
    std::vector<std::vector<KeyId>> mColumnKeyIds;

    /// Set of KeyIds belonging to `Type::time` columns at the moment the
    /// streaming pipeline took its snapshot of `Configuration()`. Populated
    /// by `BeginStreaming` and grown as `AppendBatch` performs back-fills, so
    /// each newly-promoted time column is back-filled exactly once.
    std::unordered_set<KeyId> mStageBSnapshotTimeKeys;

    /// The column range affected by the most recent `AppendBatch` back-fill,
    /// or `std::nullopt` if none. Reset on entry to every `AppendBatch` call.
    std::optional<std::pair<size_t, size_t>> mLastBackfillRange;
};

} // namespace loglib
