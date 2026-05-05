#pragma once

#include "key_index.hpp"
#include "line_source.hpp"
#include "log_configuration.hpp"
#include "log_data.hpp"
#include "log_file.hpp"
#include "log_parse_sink.hpp"

#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace loglib
{

/// `LogTable` exposes a single row range backed by `std::vector<LogLine>`
/// for both static (`FileLineSource`) and live-tail (`StreamLineSource`)
/// sessions. Value resolution dispatches through each row's
/// `LineSource *`.
class LogTable
{
public:
    LogTable() = default;

    LogTable(LogData data, LogConfigurationManager configuration);

    LogTable(LogTable &) = delete;
    LogTable &operator=(const LogTable &) = delete;

    LogTable(LogTable &&) = default;
    LogTable &operator=(LogTable &&) = default;

    /// Replaces the table's data with a freshly-merged @p data.
    void Update(LogData &&data);

    /// Clears `Data()` and streaming-time-key snapshots; preserves `Configuration()`.
    void Reset();

    /// Init the table for a streaming parse and snapshot time-column
    /// KeyIds against the current configuration. `Configuration()`
    /// must not be mutated until the matching streamingFinished signal.
    /// Takes ownership of @p source for the session; works for both
    /// `FileLineSource` (static, mmap-backed) and `StreamLineSource`
    /// (live tail, internally thread-safe). @p source may be null.
    void BeginStreaming(std::unique_ptr<LineSource> source);

    /// Multi-file streaming append. Adds @p source to the existing
    /// session without resetting `mData`/`KeyIndex`/time-column
    /// snapshot; the next parse drives over @p source and routes line
    /// offsets via `LogData::BackFileSource()`. Used by
    /// `LogModel::AppendStreaming`. `source` must be non-null.
    void AppendStreaming(std::unique_ptr<LineSource> source);

    /// Splices @p batch into the table. Extends the configuration for any
    /// `batch.newKeys`, then back-fills any newly-introduced `Type::time`
    /// column over **all** rows. Resets `LastBackfillRange()` on entry.
    void AppendBatch(StreamedBatch batch);

    /// Non-mutating preview of what `AppendBatch(batch)` would expose to a
    /// `QAbstractTableModel`-style consumer once committed. Lets `LogModel`
    /// call Qt's `beginInsert{Rows,Columns}` *before* the actual mutation,
    /// honouring Qt's begin-before-mutate model contract.
    struct AppendBatchPreview
    {
        size_t newRowCount = 0;
        size_t newColumnCount = 0;
    };
    [[nodiscard]] AppendBatchPreview PreviewAppend(const StreamedBatch &batch) const;

    /// Inclusive `[firstColumn, lastColumn]` columns that the most recent
    /// `AppendBatch` back-filled, or `std::nullopt` if none. Reset on entry.
    [[nodiscard]] const std::optional<std::pair<size_t, size_t>> &LastBackfillRange() const noexcept;

    /// Reorders the column at @p srcIndex to @p destIndex in the underlying
    /// configuration and keeps the column-to-KeyId cache aligned. Callers
    /// must wrap with the corresponding Qt `beginMoveColumns` /
    /// `endMoveColumns` notifications so attached views observe the move.
    void MoveColumn(size_t srcIndex, size_t destIndex);

    /// Pre-allocation hint forwarded to the installed streaming `LogFile`; no-op otherwise.
    void ReserveLineOffsets(size_t count);

    [[nodiscard]] std::string GetHeader(size_t column) const;
    [[nodiscard]] size_t ColumnCount() const;
    [[nodiscard]] LogValue GetValue(size_t row, size_t column) const;
    [[nodiscard]] std::string GetFormattedValue(size_t row, size_t column) const;
    [[nodiscard]] size_t RowCount() const;

    [[nodiscard]] const LogData &Data() const noexcept;
    [[nodiscard]] LogData &Data() noexcept;

    /// Drop the first @p count rows. Used by `LogModel`'s FIFO
    /// retention; callers must wrap with `beginRemoveRows` /
    /// `endRemoveRows`. `count == 0` and `count >= RowCount()` are
    /// both well-defined.
    void EvictPrefixRows(size_t count);

    /// Mutable `KeyIndex` for worker-thread `GetOrInsert` (used by `QtStreamingLogSink`).
    KeyIndex &Keys();
    const KeyIndex &Keys() const;

    const LogConfigurationManager &Configuration() const;
    /// Non-const access for `Load`/`Save` menu actions. Must not be mutated
    /// mid-streaming; `MainWindow` gates the configuration-edit UI accordingly.
    LogConfigurationManager &Configuration();

private:
    static std::string FormatLogValue(const std::string &format, const LogValue &value);

    void RefreshColumnKeyIds();
    void RefreshColumnKeyIdsForKeys(const std::vector<std::string> &newKeys);
    void RefreshSnapshotTimeKeys();

    LogData mData;
    LogConfigurationManager mConfiguration;
    std::vector<std::vector<KeyId>> mColumnKeyIds;

    /// `Type::time` KeyIds in the configuration at `BeginStreaming`
    /// time; promoted inline by Stage B.
    std::unordered_set<KeyId> mStageBSnapshotTimeKeys;

    /// `Type::time` KeyIds that appeared after the snapshot (e.g.
    /// auto-promoted on first sight). Stage B does not know about
    /// them, so every `AppendBatch` back-fills the rows it appended.
    std::unordered_set<KeyId> mPostSnapshotTimeKeys;

    std::optional<std::pair<size_t, size_t>> mLastBackfillRange;
};

} // namespace loglib
