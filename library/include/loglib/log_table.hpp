#pragma once

#include "enum_dictionary.hpp"
#include "key_index.hpp"
#include "line_source.hpp"
#include "log_configuration.hpp"
#include "log_data.hpp"
#include "log_file.hpp"
#include "log_parse_sink.hpp"

#include <cstdint>
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

    // Out-of-line so the move re-runs `RewireSourceRegistries()`:
    // every `LineSource` caches a pointer to `mEnumDictionaries`, and a
    // defaulted move would leave those pointing at the moved-from
    // address.
    LogTable(LogTable &&) noexcept;
    LogTable &operator=(LogTable &&) noexcept;

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

    /// Per-column enum dictionaries. Filter UI reads
    /// `Find(keyId)->Values()` for the multi-select picker.
    const EnumDictionaryRegistry &EnumDictionaries() const noexcept;

    /// Override the per-column distinct-value cap. Clamped to
    /// `[1, MAX_ENUM_VALUES]`. No effect on existing dictionaries.
    void SetEnumValueCap(uint16_t cap) noexcept;

    [[nodiscard]] uint16_t EnumValueCap() const noexcept;

    /// Dictionary id for the slot at @p row / @p column iff it is a
    /// `DictRef`; nullopt otherwise. Powers the model's
    /// `EnumValueRole` fast-filter path.
    [[nodiscard]] std::optional<EnumValueId> GetEnumValueId(size_t row, size_t column) const noexcept;

    const LogConfigurationManager &Configuration() const;
    /// Non-const access for `Load`/`Save` menu actions. Must not be mutated
    /// mid-streaming; `MainWindow` gates the configuration-edit UI accordingly.
    LogConfigurationManager &Configuration();

private:
    /// Per-column observed-value tracker for enum auto-detection.
    /// Tracks up to `cap` distinct values; the (cap+1)th flips `killed`
    /// and drops the buffers.
    struct EnumCandidateTracker
    {
        /// Heterogeneous hasher so `string_view` lookups skip the temp
        /// `std::string`.
        struct StringHash
        {
            // NOLINTNEXTLINE(readability-identifier-naming): standard
            // library tag name required for heterogeneous lookup.
            using is_transparent = void;
            [[nodiscard]] size_t operator()(std::string_view s) const noexcept
            {
                return std::hash<std::string_view>{}(s);
            }
            [[nodiscard]] size_t operator()(const std::string &s) const noexcept
            {
                return std::hash<std::string_view>{}(std::string_view(s));
            }
        };

        std::vector<std::string> values;
        std::unordered_set<std::string, StringHash, std::equal_to<>> seen;
        uint16_t size = 0;
        uint16_t cap = DEFAULT_ENUM_VALUE_CAP;
        size_t rowsObserved = 0;
        bool killed = false;

        EnumCandidateTracker() = default;
        explicit EnumCandidateTracker(uint16_t capValue) noexcept
            : cap(capValue)
        {
            values.reserve(capValue);
            seen.reserve(capValue);
        }

        /// True iff @p bytes was new (grew the distinct set). Flips
        /// `killed` when a new value pushes `size` past `cap`.
        bool Observe(std::string_view bytes);
    };

    static std::string FormatLogValue(const std::string &format, const LogValue &value);

    void RefreshColumnKeyIds();
    void RefreshColumnKeyIdsForKeys(const std::vector<std::string> &newKeys);
    void RefreshSnapshotTimeKeys();
    void RefreshSnapshotEnumKeys();

    /// Point every owned `LineSource` at `mEnumDictionaries`.
    void RewireSourceRegistries();

    /// Enum pass over rows `[oldLineCount, Lines().size())`. Updates
    /// trackers, demotes overflowing columns, encodes the slice for
    /// already-promoted columns, and auto-promotes quiescent
    /// candidates. Extends @p firstBackfilled / @p lastBackfilled when
    /// table-wide work is done.
    void RunEnumPassForAppendBatch(
        size_t oldLineCount, std::optional<size_t> &firstBackfilled, std::optional<size_t> &lastBackfilled
    );

    /// Promote @p columnIndex from `Type::any` to `Type::enumeration`,
    /// encoding every existing row's slot as `DictRef`.
    void PromoteColumnToEnum(size_t columnIndex);

    /// Demote @p columnIndex from `Type::enumeration` back to
    /// `Type::any`, materialising every `DictRef` to `OwnedString` and
    /// dropping the dictionary.
    void DemoteColumnFromEnum(size_t columnIndex);

    /// Encode the column's slots in `[rowBegin, rowEnd)` as `DictRef`,
    /// growing the dictionary as needed. Returns false if the
    /// dictionary would exceed `MAX_ENUM_VALUES`; caller should demote.
    bool EncodeColumnRangeAsEnum(const LogConfiguration::Column &column, size_t rowBegin, size_t rowEnd);

    /// Shared inner loop. @p keyIds must contain only resolved entries.
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

    /// Per-column dictionaries for `Type::enumeration` columns. Every
    /// owned `LineSource` points at this registry.
    EnumDictionaryRegistry mEnumDictionaries;

    /// Effective per-column distinct-value cap; stamped on new
    /// dictionaries and trackers.
    uint16_t mEnumValueCap = DEFAULT_ENUM_VALUE_CAP;

    /// Trackers for promotion candidates, keyed on `column.keys.front()`
    /// (the configured canonical key string -- stable across batches
    /// even when alias keys arrive out of order). Live only while the
    /// column is still `Type::any` and not yet killed.
    std::unordered_map<std::string, EnumCandidateTracker> mEnumTrackers;

    /// Canonical-key strings for columns that have overflowed the cap
    /// at least once. Skipped by `RunEnumPassForAppendBatch` to avoid
    /// promote/demote oscillation. Cleared by `Reset()`.
    std::unordered_set<std::string> mEnumPermanentlyKilled;

    /// Scratch buffer reused across the per-column inner loop in
    /// `RunEnumPassForAppendBatch`.
    std::vector<KeyId> mScratchResolvedKeyIds;

    std::optional<std::pair<size_t, size_t>> mLastBackfillRange;
};

} // namespace loglib
