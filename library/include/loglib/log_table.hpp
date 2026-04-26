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
    /**
     * @brief Default constructor for LogTable.
     */
    LogTable() = default;

    /**
     * @brief Constructs a LogTable with the specified data and configuration.
     *
     * @param data The log data to be stored in the table.
     * @param configuration The configuration manager that controls how log data is displayed.
     */
    LogTable(LogData data, LogConfigurationManager configuration);

    // Deleted copy constructor and copy assignment operator for efficiency.
    LogTable(LogTable &) = delete;
    LogTable &operator=(const LogTable &) = delete;

    // Defaulted move constructor and move assignment operator.
    LogTable(LogTable &&) = default;
    LogTable &operator=(LogTable &&) = default;

    /**
     * @brief Updates the table with new log data.
     *
     * @param data The new log data to be merged with the existing data.
     */
    void Update(LogData &&data);

    /**
     * @brief Initialises the table for an upcoming streaming parse.
     *
     * Installs @p file as the data source (so subsequent `AppendBatch` calls
     * can splice batches in without owning a separate `LogData` per batch),
     * snapshots the time-column KeyId set against the configuration that the
     * Stage B parser is about to be handed (PRD req. 4.1.13b / 4.2.21), and
     * resets `LastBackfillRange()` to `nullopt`. After this call the table is
     * ready to consume `StreamedBatch`es.
     *
     * Note that `Configuration()` is *not* mutated by `BeginStreaming`; the
     * GUI is expected to lock the configuration UI between `BeginStreaming`
     * and the corresponding `streamingFinished` signal (PRD req. 4.3.29).
     *
     * @param file Optional already-opened source file. Pass `nullptr` to keep
     *             the table file-less (e.g. fixture tests that hand-craft
     *             `StreamedBatch`es without a backing `LogFile`).
     */
    void BeginStreaming(std::unique_ptr<LogFile> file);

    /**
     * @brief Appends one streaming batch of pre-parsed lines to the table.
     *
     * Implements the streaming append path from PRD req. 4.1.13a / 4.2.18
     * Stage C. Behaviour:
     *   - Resets `LastBackfillRange()` to `nullopt` at entry.
     *   - If `batch.newKeys` is non-empty, extends the configuration's column
     *     list (always at the end — PRD req. 4.1.13) so any unmapped key
     *     appears as either a freshly-promoted `Type::time` column (when the
     *     auto-promotion heuristic matches) or a default `Type::any` column.
     *   - Splices `batch.lines` and `batch.localLineOffsets` into the owned
     *     `LogData` via `LogData::AppendBatch`.
     *   - For each `Type::time` column whose KeyId set is *not* a subset of
     *     the snapshot stamped by `BeginStreaming`, runs
     *     `BackfillTimestampColumn` over **all** rows (already-appended +
     *     just-appended) so the new column has parsed timestamps everywhere
     *     (PRD req. 4.1.13b). Updates the snapshot so subsequent batches do
     *     not re-back-fill the same column. Records the affected column range
     *     in `mLastBackfillRange` for `LogModel::AppendBatch` to drive a
     *     `dataChanged` notification.
     *
     * The function is intentionally `void`: all column / row count deltas
     * needed by Qt's `beginInsertRows`/`beginInsertColumns` are observed by
     * the caller via `RowCount()`/`ColumnCount()` (PRD third-pass review,
     * Decision 33).
     *
     * @param batch Streaming batch to consume. Move-out is encouraged.
     */
    void AppendBatch(StreamedBatch batch);

    /**
     * @brief Returns the inclusive `[firstColumn, lastColumn]` range of
     *        columns that the most recent `AppendBatch` back-filled
     *        timestamps for, or `std::nullopt` if no back-fill happened.
     *
     * Reset to `std::nullopt` at the start of every `AppendBatch` call. Used
     * by `LogModel::AppendBatch` to emit `dataChanged` for the affected cells
     * (PRD req. 4.1.13a, 4.3.27).
     */
    const std::optional<std::pair<size_t, size_t>> &LastBackfillRange() const;

    /**
     * @brief Gets the header text for the specified column.
     *
     * @param column The index of the column.
     * @return The header text as a string.
     */
    std::string GetHeader(size_t column) const;

    /**
     * @brief Gets the total number of columns in the table.
     *
     * @return The number of columns.
     */
    size_t ColumnCount() const;

    /**
     * @brief Gets the raw log value at the specified row and column.
     *
     * @param row The index of the row.
     * @param column The index of the column.
     * @return The log value at the specified position.
     */
    LogValue GetValue(size_t row, size_t column) const;

    /**
     * @brief Gets the formatted string representation of the value at the specified row and column.
     *
     * @param row The index of the row.
     * @param column The index of the column.
     * @return The formatted string representation of the value.
     */
    std::string GetFormattedValue(size_t row, size_t column) const;

    /**
     * @brief Gets the total number of rows in the table.
     *
     * @return The number of rows.
     */
    size_t RowCount() const;

    /**
     * @brief Gets a const reference to the underlying log data.
     *
     * @return A const reference to the log data.
     */
    const LogData &Data() const;

    /**
     * @brief Gets a mutable reference to the underlying log data.
     *
     * @return A mutable reference to the log data.
     */
    LogData &Data();

    /**
     * @brief Gets a const reference to the configuration manager.
     *
     * @return A const reference to the configuration manager.
     */
    const LogConfigurationManager &Configuration() const;

    /**
     * @brief Gets a mutable reference to the configuration manager.
     *
     * @return A mutable reference to the configuration manager.
     */
    LogConfigurationManager &Configuration();

private:
    /**
     * @brief Formats a log value according to the specified format string.
     *
     * @param format The format string to use.
     * @param value The log value to format.
     * @return The formatted string representation of the value.
     */
    static std::string FormatLogValue(const std::string &format, const LogValue &value);

    /**
     * @brief Rebuilds the column → KeyId cache from the current configuration.
     *
     * The cache is one inner vector per configured column. Each inner vector
     * lists the canonical KeyIds of the configuration's `column.keys`,
     * resolved through the owning `LogData::Keys()`. Unknown keys are stored
     * as `kInvalidKeyId` and skipped at lookup time. Called whenever the data
     * or configuration changes wholesale — i.e. from `Update`, the constructor,
     * and `BeginStreaming` (PRD req. 4.1.12).
     */
    void RefreshColumnKeyIds();

    /**
     * @brief Incrementally patches the column → KeyId cache for columns
     *        that reference any key in @p newKeys.
     *
     * Used by `AppendBatch` when the streaming pipeline reports new keys
     * (`StreamedBatch::newKeys`) — the only situation where the
     * `mColumnKeyIds` cache built by the previous `RefreshColumnKeyIds` /
     * `RefreshColumnKeyIdsForKeys` pass can have gone stale, because
     * `KeyIndex::Find` may have started returning a valid KeyId for a key
     * that previously resolved to `kInvalidKeyId`. Columns whose `keys`
     * vector has no overlap with @p newKeys keep their cached entries
     * untouched: per PRD §4.2 the canonical KeyIndex is monotonic
     * (KeyIds are dense and never reassigned), so once a column's keys
     * have been resolved they stay resolved for the rest of the parse.
     *
     * For a 100-column streaming parse with 1 000 batches and zero new
     * keys after batch 1, this combined with the `!batch.newKeys.empty()`
     * gate in `AppendBatch` saves ~99 000 redundant `KeyIndex::Find`
     * calls on the GUI thread (PRD §4.8.2 worked example).
     *
     * @param newKeys Keys observed for the first time in the most recent
     *                streaming batch — the same vector passed via
     *                `StreamedBatch::newKeys` and forwarded to
     *                `LogConfigurationManager::AppendKeys`.
     */
    void RefreshColumnKeyIdsForKeys(const std::vector<std::string> &newKeys);

    /**
     * @brief Internal shared body of `Update` and the configuration-snapshot
     *        captured by `BeginStreaming`.
     */
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
