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
#include <span>
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

    /// Test/tuning hook: override the per-column distinct-value cap.
    /// Clamped to `[1, MAX_ENUM_VALUES]`. No effect on existing
    /// dictionaries. Not exposed via the GUI; tests use it to keep
    /// fixtures small.
    void SetEnumValueCap(uint16_t cap) noexcept;

    [[nodiscard]] uint16_t EnumValueCap() const noexcept;

    /// Test/tuning hook: override the per-value byte-length cap used
    /// by the health policy. `0` disables the cap. Long values are
    /// not killed on first sight; the percentile tolerance in
    /// `RunEnumPassForAppendBatch` decides whether the column gets
    /// demoted.
    void SetEnumValueMaxLen(uint32_t maxLen) noexcept;

    [[nodiscard]] uint32_t EnumValueMaxLen() const noexcept;

    /// Dictionary id for the slot at @p row / @p column iff it is a
    /// `DictRef`; nullopt otherwise. Powers the model's
    /// `EnumValueRole` fast-filter path.
    [[nodiscard]] std::optional<EnumValueId> GetEnumValueId(size_t row, size_t column) const noexcept;

    /// End-of-parse / end-of-stream auto-detection sweep.
    ///
    /// `RunEnumPassForAppendBatch`'s per-batch promotion gate uses
    /// the `ENUM_PROMOTION_MIN_ROWS{,_WELL_KNOWN}` thresholds (or
    /// `STREAM_PROMOTION_MIN_ROWS` in stream mode). Files smaller than
    /// the static threshold and streams that cancel between batches
    /// would otherwise leave their candidates languishing as
    /// `Type::unknown`. This pass runs the permissive promotion rule
    /// (`size in [1, cap] && rowsObserved >= 2 && !killed`) against
    /// the surviving trackers, then transitions any remaining
    /// `Type::unknown` columns to a terminal type based on what the
    /// candidate scan saw.
    ///
    /// Called from the static-parse constructor and from
    /// `LogModel::EndStreaming(false)` (i.e. only on graceful end;
    /// cancellation skips the sweep). Idempotent. Clears
    /// `mEnumTrackers` and `mIsStreaming` on exit.
    ///
    /// Returns `true` iff at least one column flipped to
    /// `Type::enumeration` during the sweep. `LogModel::EndStreaming`
    /// uses this to gate the `enumColumnsChanged` signal so a clean
    /// stream end with no new enum columns does not spuriously
    /// rebuild the filter editor's enum picker.
    bool FinalizeAutoDetection();

    const LogConfigurationManager &Configuration() const;
    /// Non-const access for `Load`/`Save` menu actions. Must not be mutated
    /// mid-streaming; `MainWindow` gates the configuration-edit UI accordingly.
    LogConfigurationManager &Configuration();

private:
    /// Per-column observed-value tracker for enum auto-detection.
    /// Tracks up to `cap` distinct values; the (cap+1)th flips `killed`
    /// and drops the buffer (hard cap, no tolerance — there is no way
    /// to encode a (cap+2)th value into a fixed dictionary). A value
    /// longer than `valueMaxLen` (when non-zero) is counted in
    /// `longValueCount`; the column is killed only when the long-value
    /// fraction exceeds `ENUM_HEALTH_TOLERANCE_RATIO` past
    /// `ENUM_HEALTH_MIN_SAMPLES` presences. Single stray long values
    /// no longer disqualify the column.
    ///
    /// Uses a flat `std::vector<std::string>` for both storage and
    /// membership: at the small caps in play (default 64) a linear
    /// scan stays L1-resident and beats `unordered_set` on hash + heap
    /// overhead — most observations are existing values, hit early in
    /// the scan.
    ///
    /// `intObservations`, `uintObservations`, and `doubleObservations`
    /// count Int64, UInt64, and Double tags seen in the candidate scan.
    /// The no-string bail uses these to route a column out of
    /// `unknown`: int / uint -> `Type::integer`; double -> `Type::floating`;
    /// any mix of integral and floating -> `Type::number`; neither
    /// (all nulls / bools) -> `Type::any`. The signed/unsigned split is
    /// kept in the counters so future numeric widgets can differentiate
    /// without a wire-format change; today's routing collapses both to
    /// `Type::integer`.
    ///
    /// `presenceCount` counts rows where the slot was actually present
    /// (any non-monostate tag, including DictRef). `rowsObserved` counts
    /// loop progress (every row scanned, present or absent). The split
    /// keeps sparse columns out of the no-string bail until they have
    /// actually been seen.
    struct EnumCandidateTracker
    {
        std::vector<std::string> values;
        uint32_t valueMaxLen = 0;
        uint16_t size = 0;
        uint16_t cap = DEFAULT_ENUM_VALUE_CAP;
        size_t rowsObserved = 0;
        size_t presenceCount = 0;
        size_t longValueCount = 0;
        size_t intObservations = 0;
        size_t uintObservations = 0;
        size_t doubleObservations = 0;
        bool killed = false;

        EnumCandidateTracker() = default;
        EnumCandidateTracker(uint16_t capValue, uint32_t valueMaxLenValue) noexcept
            : valueMaxLen(valueMaxLenValue), cap(capValue)
        {
            values.reserve(capValue);
        }

        /// Caller has already incremented `presenceCount` for this row.
        /// Updates `values` / `size`, counts long values into
        /// `longValueCount`, and flips `killed` when the long-value
        /// percentile exceeds `ENUM_HEALTH_TOLERANCE_RATIO` past
        /// `ENUM_HEALTH_MIN_SAMPLES` presences, or when a (cap+1)th
        /// distinct value arrives.
        void Observe(std::string_view bytes);
    };

    /// Cumulative health for an active `Type::enumeration` column.
    /// `EncodeColumnRange` updates these counters per encoded slot;
    /// `RunEnumPassForAppendBatch` consults the tolerance ratio to
    /// decide whether to demote the column back to `Type::string`.
    /// Long values and wrong-type slots both accrue against the same
    /// budget — a dictionary that started seeing strings but is now
    /// receiving numbers is just as suspect as one accumulating
    /// over-cap-length values.
    struct EnumColumnHealth
    {
        size_t totalSlots = 0;
        size_t longValueSlots = 0;
        size_t wrongTypeSlots = 0;

        [[nodiscard]] bool ShouldDemote(double tolerance, size_t minSamples) const noexcept;
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
    /// growing the dictionary as needed. Returns false only on the
    /// hard dictionary-cap overflow (no tolerance for that path);
    /// long values and wrong-type slots accumulate in @p health, and
    /// the caller decides whether the percentile threshold has been
    /// crossed.
    bool EncodeColumnRangeAsEnum(
        const LogConfiguration::Column &column, size_t rowBegin, size_t rowEnd, EnumColumnHealth &health
    );

    /// Shared inner loop. @p aliasKeys must contain only resolved
    /// entries; the first one is the canonical dictionary key
    /// (aliases share its dictionary). Updates @p health with
    /// `totalSlots` / `longValueSlots` / `wrongTypeSlots`; returns
    /// false on hard dictionary-cap overflow.
    bool EncodeColumnRange(std::span<const KeyId> aliasKeys, size_t rowBegin, size_t rowEnd, EnumColumnHealth &health);

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

    /// Effective per-value byte-length cap for auto-discovered
    /// columns. `0` disables the cap.
    uint32_t mEnumValueMaxLen = MAX_ENUM_CANDIDATE_LEN;

    /// Trackers for promotion candidates, keyed on `column.header`
    /// (unique per column and stable across alias-list edits). Live
    /// only while the column is still `Type::unknown`. Cleared by
    /// `FinalizeAutoDetection` and `Reset`.
    std::unordered_map<std::string, EnumCandidateTracker> mEnumTrackers;

    /// Cumulative health counters for active `Type::enumeration`
    /// columns, keyed on `column.header`. Survives across batches
    /// (so the percentile tolerance reflects the full session, not
    /// the last batch). Erased on `DemoteColumnFromEnum` and `Reset`.
    std::unordered_map<std::string, EnumColumnHealth> mEnumColumnHealth;

    /// True between `BeginStreaming` and `FinalizeAutoDetection`. When
    /// set, `RunEnumPassForAppendBatch` uses the smaller stream
    /// promotion threshold and skips the cardinality bail (live UI
    /// wants enum chips to appear after a couple of rows; demote tax
    /// is bounded). Static parses leave this false.
    bool mIsStreaming = false;

    std::optional<std::pair<size_t, size_t>> mLastBackfillRange;
};

} // namespace loglib
