#pragma once

#include "key_index.hpp"
#include "log_configuration.hpp"
#include "log_data.hpp"
#include "log_file.hpp"
#include "streaming_log_sink.hpp"

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

    LogTable(LogData data, LogConfigurationManager configuration);

    LogTable(LogTable &) = delete;
    LogTable &operator=(const LogTable &) = delete;

    LogTable(LogTable &&) = default;
    LogTable &operator=(LogTable &&) = default;

    /// Replaces the table's data with a freshly-merged @p data.
    void Update(LogData &&data);

    /// Data-only reset: clears `Data()` and the streaming-time-key snapshots,
    /// but leaves `Configuration()` (the user-loaded columns / formats /
    /// filters) untouched. `LogModel::Clear()` uses this so a `File → Open` /
    /// drag-and-drop after `LoadConfiguration` does not silently discard the
    /// user's column layout.
    void Reset();

    /// Initialises the table for an upcoming streaming parse and snapshots the
    /// time-column KeyIds against the current configuration. `Configuration()`
    /// is *not* mutated; callers must lock the configuration UI between this
    /// call and the matching streaming-finished signal. @p file may be null.
    void BeginStreaming(std::unique_ptr<LogFile> file);

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

    /// Forwards to `Data().Files().front()->ReserveLineOffsets(count)` if a
    /// streaming `LogFile` is installed; no-op otherwise. Avoids handing the
    /// model a non-const `LogData&` accessor for what should be a one-line
    /// pre-allocation hint.
    void ReserveLineOffsets(size_t count);

    [[nodiscard]] std::string GetHeader(size_t column) const;
    [[nodiscard]] size_t ColumnCount() const;
    [[nodiscard]] LogValue GetValue(size_t row, size_t column) const;
    [[nodiscard]] std::string GetFormattedValue(size_t row, size_t column) const;
    [[nodiscard]] size_t RowCount() const;

    [[nodiscard]] const LogData &Data() const noexcept;

    /// Mutable `KeyIndex` reference used by `QtStreamingLogSink` so it can
    /// `GetOrInsert` field names from worker threads. Replaces the previous
    /// mutable `Data()` accessor, which incidentally exposed every other
    /// `LogData` member to external mutation.
    KeyIndex &Keys();
    const KeyIndex &Keys() const;

    const LogConfigurationManager &Configuration() const;
    /// Non-const access is intentionally retained for the configuration
    /// `Load` / `Save` menu actions wired through `LogModel`. Mutating the
    /// configuration mid-streaming is forbidden — `MainWindow` gates the
    /// configuration-edit UI for the lifetime of the active parse.
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
