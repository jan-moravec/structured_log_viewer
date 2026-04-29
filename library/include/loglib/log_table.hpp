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

    LogTable(LogData data, LogConfigurationManager configuration);

    LogTable(LogTable &) = delete;
    LogTable &operator=(const LogTable &) = delete;

    LogTable(LogTable &&) = default;
    LogTable &operator=(LogTable &&) = default;

    /// Replaces the table's data with a freshly-merged @p data.
    void Update(LogData &&data);

    /// Initialises the table for an upcoming streaming parse and snapshots the
    /// time-column KeyIds against the current configuration. `Configuration()`
    /// is *not* mutated; callers must lock the configuration UI between this
    /// call and the matching streaming-finished signal. @p file may be null.
    void BeginStreaming(std::unique_ptr<LogFile> file);

    /// Splices @p batch into the table. Extends the configuration for any
    /// `batch.newKeys`, then back-fills any newly-introduced `Type::time`
    /// column over **all** rows. Resets `LastBackfillRange()` on entry.
    void AppendBatch(StreamedBatch batch);

    /// Inclusive `[firstColumn, lastColumn]` columns that the most recent
    /// `AppendBatch` back-filled, or `std::nullopt` if none. Reset on entry.
    const std::optional<std::pair<size_t, size_t>> &LastBackfillRange() const;

    std::string GetHeader(size_t column) const;
    size_t ColumnCount() const;
    LogValue GetValue(size_t row, size_t column) const;
    std::string GetFormattedValue(size_t row, size_t column) const;
    size_t RowCount() const;

    const LogData &Data() const;
    LogData &Data();

    const LogConfigurationManager &Configuration() const;
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
    /// `RefreshSnapshotTimeKeys` — which `GetOrInsert`s into the freshly
    /// empty `KeyIndex` so the snapshot can hold valid ids before the
    /// parser pipeline's `BuildTimeColumnSpecs` runs. Read-only thereafter.
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
