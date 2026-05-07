#pragma once

#include "enum_dictionary.hpp"
#include "key_index.hpp"
#include "line_source.hpp"
#include "log_configuration.hpp"
#include "log_data.hpp"
#include "log_file.hpp"
#include "log_parse_sink.hpp"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
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

    /// Read-only access to the per-column enum dictionaries. Filter UI
    /// reads `Find(keyId)->Values()` to populate the multi-select picker.
    const EnumDictionaryRegistry &EnumDictionaries() const noexcept;

    const LogConfigurationManager &Configuration() const;
    /// Non-const access for `Load`/`Save` menu actions. Must not be mutated
    /// mid-streaming; `MainWindow` gates the configuration-edit UI accordingly.
    LogConfigurationManager &Configuration();

private:
    /// Per-column "is this column still a candidate?" tracker. Tracks
    /// up to `MAX_ENUM_VALUES + 1` distinct observed values; once the
    /// `+1`th distinct value lands, the column is no longer a candidate
    /// and the slot stays empty until `Reset`.
    struct EnumCandidateTracker
    {
        std::array<std::string, MAX_ENUM_VALUES + 1U> values{};
        uint16_t size = 0;
        size_t rowsObserved = 0;
        bool killed = false; ///< Tracker exceeded the cap; column is not promotable.

        /// Observes @p bytes. Returns true iff @p bytes was new (strictly
        /// grew the distinct-value set). Side effect: flips `killed` when
        /// a new value pushes `size` past `MAX_ENUM_VALUES`.
        bool Observe(std::string_view bytes);
    };

    static std::string FormatLogValue(const std::string &format, const LogValue &value);

    void RefreshColumnKeyIds();
    void RefreshColumnKeyIdsForKeys(const std::vector<std::string> &newKeys);
    void RefreshSnapshotTimeKeys();
    void RefreshSnapshotEnumKeys();

    /// Hook every line source to `mEnumDictionaries`. Called whenever a
    /// new source enters the session.
    void RewireSourceRegistries();

    /// Run the enum pass over the most recent batch (rows
    /// `[oldLineCount, mData.Lines().size())`). Updates trackers,
    /// performs reactive demotion, encode-if-fits, and quiescence-based
    /// auto-promotion. Updates @p firstBackfilled / @p lastBackfilled
    /// when columns are touched table-wide.
    void RunEnumPassForAppendBatch(
        size_t oldLineCount, std::optional<size_t> &firstBackfilled, std::optional<size_t> &lastBackfilled
    );

    /// Auto-promote @p columnIndex (currently `Type::any`) to
    /// `Type::enumeration`. Walks all rows, encodes every slot for the
    /// column's keys as `DictRef`, and flips the configuration entry.
    void PromoteColumnToEnum(size_t columnIndex);

    /// Demote @p columnIndex (currently `Type::enumeration`) back to
    /// `Type::any`. Walks all rows, materialises every `DictRef` slot
    /// to `OwnedString`, and flips the configuration entry. The
    /// dictionary entry for the column's keys is dropped.
    void DemoteColumnFromEnum(size_t columnIndex);

    /// Resolve the column's bytes from the row at @p rowIndex (any
    /// `MmapSlice` / `OwnedString` / `DictRef` slot maps to a stable
    /// view valid for the duration of the call). Returns nullopt when
    /// the row has no value for any of the column's keys.
    std::optional<std::string> ResolveColumnBytes(const LogConfiguration::Column &column, size_t rowIndex) const;

    /// Encode the column's slot in the new-batch slice as `DictRef`,
    /// growing the dictionary if necessary. Returns false when the
    /// dictionary would have to grow past `MAX_ENUM_VALUES`; caller
    /// should demote instead.
    bool EncodeColumnRangeAsEnum(const LogConfiguration::Column &column, size_t rowBegin, size_t rowEnd);

    /// Inner loop shared by the column-by-name and column-by-cached-id
    /// entry points. @p keyIds must contain only resolved (`KeyId !=
    /// INVALID_KEY_ID`) entries.
    bool EncodeColumnRange(const std::vector<KeyId> &keyIds, size_t rowBegin, size_t rowEnd);

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

    /// Per-column dictionaries for `Type::enumeration` columns.
    /// `LineSource::EnumDictionaries()` points at this registry on
    /// every line source in the current session.
    EnumDictionaryRegistry mEnumDictionaries;

    /// Per-column observed-value trackers used by the auto-detector.
    /// Keyed by KeyId so that columns aliasing multiple keys still
    /// share state. Kept only while the column is still a promotion
    /// candidate (`Type::any` and tracker not killed).
    std::unordered_map<KeyId, EnumCandidateTracker> mEnumTrackers;

    std::optional<std::pair<size_t, size_t>> mLastBackfillRange;
};

} // namespace loglib
