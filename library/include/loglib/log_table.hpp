#pragma once

#include "file_line_source.hpp"
#include "key_index.hpp"
#include "log_configuration.hpp"
#include "log_data.hpp"
#include "log_file.hpp"
#include "stream_line_source.hpp"
#include "streaming_log_sink.hpp"

#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace loglib
{

/// Row dispatch:
///
/// `LogTable` exposes a single logical row range backed by a single
/// `std::vector<LogLine>` regardless of whether the session was opened
/// from a static file (`FileLineSource`) or a live stream
/// (`StreamLineSource`). Each `LogLine` carries its own `LineSource *`
/// so value resolution stays uniform across both paths.
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

    /// Initialises the table for an upcoming static-streaming parse and
    /// snapshots the time-column KeyIds against the current configuration.
    /// `Configuration()` is *not* mutated; callers must lock the
    /// configuration UI between this call and the matching streaming-
    /// finished signal. @p source may be null (`mData` is reset to empty).
    /// `LogTable` takes ownership of @p source for the lifetime of the
    /// session; the parser worker emits `LogLine`s tagged with that source
    /// so each row's raw bytes stay resolvable through
    /// `LineSource::RawLine` after parsing has moved on.
    void BeginStreaming(std::unique_ptr<FileLineSource> source);

    /// `BeginStreaming` overload for the live-tail / non-mmap path.
    /// `LogTable` takes ownership of @p source so the parser worker
    /// can mutate it via `AppendLine` / `AppendOwnedBytes` while the
    /// GUI thread reads through `LogLine::Source()->RawLine` /
    /// `ResolveOwnedBytes` (StreamLineSource is internally
    /// thread-safe). The session is reset to "stream-only": no
    /// `FileLineSource` is installed, and `Lines()` is initially
    /// empty. @p source may be null (`mData` is reset to empty).
    void BeginStreaming(std::unique_ptr<StreamLineSource> source);

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

    /// Drop the first @p count rows from the underlying row vectors. The
    /// logical row range is `[file rows][stream rows]`, so file rows are
    /// dropped first; once they are exhausted the eviction continues
    /// into the stream rows. Used by `LogModel`'s FIFO retention-cap
    /// machinery (PRD 4.5 / 4.10.3); callers must wrap with the
    /// corresponding Qt `beginRemoveRows` / `endRemoveRows` notifications
    /// so attached views observe the prefix removal.
    ///
    /// `count == 0` and `count >= RowCount()` are both well-defined:
    /// the latter clears every row in source-order across both vectors.
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

    /// KeyIds of `Type::time` columns that were already in the configuration
    /// snapshot at `BeginStreaming` time (and are therefore promoted inline
    /// by Stage B of the parser pipeline). Populated once in
    /// `RefreshSnapshotTimeKeys`, which `GetOrInsert`s into the fresh
    /// `KeyIndex` so the snapshot holds valid ids before Stage B runs.
    std::unordered_set<KeyId> mStageBSnapshotTimeKeys;

    /// KeyIds of `Type::time` columns that appeared *after* the streaming
    /// snapshot — typically because `LogConfigurationManager::AppendKeys`
    /// auto-promoted a freshly-seen "timestamp"/"time"/"t" key on the first
    /// batch that carried it. Stage B of the running parser does not know
    /// about them (the `LogConfiguration` it received does not list the
    /// column), so every subsequent `AppendBatch` must back-fill the rows
    /// it just appended for these columns.
    std::unordered_set<KeyId> mPostSnapshotTimeKeys;

    std::optional<std::pair<size_t, size_t>> mLastBackfillRange;
};

} // namespace loglib
