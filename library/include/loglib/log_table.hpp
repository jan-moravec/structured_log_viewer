#pragma once

#include "enum_dictionary.hpp"
#include "internal/transparent_string_hash.hpp"
#include "key_index.hpp"
#include "line_source.hpp"
#include "log_configuration.hpp"
#include "log_data.hpp"
#include "log_file.hpp"
#include "log_level.hpp"
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

/// Row-range backed by `std::vector<LogLine>` for static and live-tail
/// sessions. Each row's `LineSource *` resolves its values.
class LogTable
{
public:
    LogTable() = default;

    LogTable(LogData data, LogConfigurationManager configuration);

    LogTable(LogTable &) = delete;
    LogTable &operator=(const LogTable &) = delete;

    /// Move re-runs `RewireSourceRegistries()` because each `LineSource`
    /// caches a pointer to `mEnumDictionaries`. Per-batch bookkeeping
    /// (notably `mLastBatchDemotedKeys`) follows the move so a table
    /// moved between `AppendBatch` and the `LogModel`-side consumer
    /// keeps a faithful `Demoted`-reason trail.
    LogTable(LogTable &&) noexcept;
    LogTable &operator=(LogTable &&) noexcept;

    /// Replaces the table's data with a freshly-merged @p data.
    void Update(LogData &&data);

    /// Clears `Data()` and streaming-time-key snapshots; preserves `Configuration()`.
    void Reset();

    /// Initialise for a streaming parse and snapshot time-column KeyIds.
    /// `Configuration()` must not mutate until streaming finishes.
    /// Takes ownership of @p source (may be null).
    void BeginStreaming(std::unique_ptr<LineSource> source);

    /// Multi-file streaming append; adds @p source without resetting.
    void AppendStreaming(std::unique_ptr<LineSource> source);

    /// Splice @p batch in, extending the configuration for any
    /// `batch.newKeys` and back-filling new `Type::Time` columns.
    void AppendBatch(StreamedBatch batch);

    /// Non-mutating preview of `AppendBatch(batch)`'s effect; lets the
    /// model call `beginInsert{Rows,Columns}` first.
    struct AppendBatchPreview
    {
        size_t newRowCount = 0;
        size_t newColumnCount = 0;
    };
    [[nodiscard]] AppendBatchPreview PreviewAppend(const StreamedBatch &batch) const;

    /// Inclusive `[firstColumn, lastColumn]` back-filled by the last
    /// `AppendBatch`, or nullopt.
    [[nodiscard]] const std::optional<std::pair<size_t, size_t>> &LastBackfillRange() const noexcept;

    /// Canonical `KeyId`s of columns demoted away from
    /// `Type::Enumeration` during the most recent `AppendBatch` /
    /// `Update` / `BeginStreaming`. Includes the silent
    /// "promoted-and-demoted-in-the-same-batch" case
    /// (`Unknown -> Enumeration -> String`) where the registry shows
    /// no dict before or after, which the `LogModel`-side
    /// `enumDictSizesBefore` snapshot can't see. Reset at the start
    /// of every batch-style call. Empty when no column demoted.
    [[nodiscard]] const std::vector<KeyId> &LastBatchDemotedKeys() const noexcept;

    /// Reorder column @p srcIndex to @p destIndex. Callers must wrap with
    /// `beginMoveColumns`/`endMoveColumns`.
    void MoveColumn(size_t srcIndex, size_t destIndex);

    /// Pre-allocation hint forwarded to the streaming `LogFile`.
    void ReserveLineOffsets(size_t count);

    [[nodiscard]] std::string GetHeader(size_t column) const;
    [[nodiscard]] size_t ColumnCount() const;
    [[nodiscard]] LogValue GetValue(size_t row, size_t column) const;
    [[nodiscard]] std::string GetFormattedValue(size_t row, size_t column) const;

    /// One-walk variant for string-predicate consumers. Resolves the
    /// slot at (@p row, @p column) once and returns its bytes:
    ///   - `string_view` slots (mmap-aliased, dictionary-resolved)
    ///     are returned directly. Valid as long as the underlying
    ///     line / source / dictionary lives.
    ///   - `std::string` slots and non-string slots (numeric, time,
    ///     bool) are written into @p buffer (via the column's
    ///     `printFormat` for numeric / time) and the view aliases
    ///     @p buffer.
    ///   - Absent slots return an empty view.
    ///
    /// Callers own @p buffer for the lifetime of the returned view.
    /// The previous `GetValue` + `GetFormattedValue` pair walked the
    /// line twice for every non-string column hit; this collapses the
    /// predicate hot path to one walk.
    [[nodiscard]] std::string_view GetValueOrFormatted(size_t row, size_t column, std::string &buffer) const;

    [[nodiscard]] size_t RowCount() const;

    [[nodiscard]] const LogData &Data() const noexcept;
    [[nodiscard]] LogData &Data() noexcept;

    /// Drop the first @p count rows; callers wrap with
    /// `beginRemoveRows`/`endRemoveRows`.
    void EvictPrefixRows(size_t count);

    /// Mutable `KeyIndex` for worker-thread `GetOrInsert`.
    KeyIndex &Keys();
    const KeyIndex &Keys() const;

    /// Per-column enum dictionaries. Filter UI reads `Find(keyId)->Values()`.
    const EnumDictionaryRegistry &EnumDictionaries() const noexcept;

    /// Test/tuning: override the per-column distinct-value cap. Clamped to
    /// `[1, MAX_ENUM_VALUES]`. No effect on existing dictionaries.
    void SetEnumValueCap(uint16_t cap) noexcept;

    [[nodiscard]] uint16_t EnumValueCap() const noexcept;

    /// Test/tuning: override the per-value byte-length cap (`0` disables).
    /// Long values accrue against the column's health budget.
    void SetEnumValueMaxLen(uint32_t maxLen) noexcept;

    [[nodiscard]] uint32_t EnumValueMaxLen() const noexcept;

    /// Dictionary id for the slot at @p row, @p column when it's a `DictRef`,
    /// else nullopt. Powers the `EnumValueRole` fast-filter path.
    [[nodiscard]] std::optional<EnumValueId> GetEnumValueId(size_t row, size_t column) const noexcept;

    /// Canonical `LogLevel` for the slot at (@p row, @p column) when the
    /// column is `Type::Level` and the slot resolves to a known level via
    /// the per-column cache. Returns `std::nullopt` for non-Level columns,
    /// monostate slots, or slots whose dictionary entry doesn't map.
    [[nodiscard]] std::optional<LogLevel> GetLevelForRow(size_t row, size_t columnIndex) const noexcept;

    /// Per-`Type::Level`-column cache mapping `EnumValueId` -> `LogLevel`.
    /// Indexed by dictionary id; the value at `id` is the canonical level
    /// for dictionary entry `id`, or `LogLevel::Unknown` if the raw string
    /// did not resolve. Empty for columns that are not `Type::Level`.
    /// Keyed by `Column::header` so column reorder is automatic.
    [[nodiscard]] const std::vector<LogLevel> *LevelRankCache(const std::string &columnHeader) const noexcept;

    /// Outcome of `ResolveEnumColumn`:
    ///   - `canonicalKey == INVALID_KEY_ID`: column out of range, has
    ///     no keys, or its first key isn't interned. Skip enum logic.
    ///   - `canonicalKey` valid, `dictionary == nullptr`: column has a
    ///     canonical key but is not currently `Type::Enumeration`.
    ///     Predicates fall back to the string-set path.
    ///   - Both populated: column is promoted; caller can use the
    ///     dictionary directly.
    struct EnumColumnLookup
    {
        KeyId canonicalKey = INVALID_KEY_ID;
        const EnumDictionary *dictionary = nullptr;
    };

    /// Resolve `column index -> canonical KeyId -> EnumDictionary*`.
    /// Returns a default-constructed lookup on any miss.
    [[nodiscard]] EnumColumnLookup ResolveEnumColumn(size_t columnIndex) const noexcept;

    /// End-of-parse/end-of-stream auto-detection sweep: promote permissive
    /// candidates and transition leftover `Type::Unknown` columns to a
    /// terminal type. Idempotent. Returns true if at least one column was
    /// promoted to `Type::Enumeration`.
    bool FinalizeAutoDetection();

    const LogConfigurationManager &Configuration() const;
    /// Non-const access for `Load`/`Save`. Must not be mutated mid-streaming.
    LogConfigurationManager &Configuration();

private:
    /// Per-column tracker for enum auto-detection. Holds up to `cap`
    /// distinct values (hard cap, no tolerance). Long values accrue
    /// in `longValueCount`; numeric-tag counters route the no-string
    /// bail to a numeric type rather than `string`. `presenceCount`
    /// and `rowsObserved` are separate so sparse columns aren't
    /// bailed before their first observation.
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    // Private nested aggregate POD: public members are intentional;
    // accessors would only obscure the per-row hot path.
    struct EnumCandidateTracker
    {
        /// Distinct values seen so far (insertion order, capped at `cap`).
        std::vector<std::string> values;
        /// O(1) membership index over `values`. Transparent hashing
        /// avoids the per-row `std::string` materialisation a
        /// non-transparent `unordered_set<string>` would force on
        /// every `string_view` lookup.
        std::unordered_set<std::string, internal::TransparentStringHash, internal::TransparentStringEqual> seen;
        uint32_t valueMaxLen = 0;
        uint16_t size = 0;
        uint16_t cap = DEFAULT_ENUM_VALUE_CAP;
        size_t rowsObserved = 0;
        size_t presenceCount = 0;
        size_t longValueCount = 0;
        size_t intObservations = 0;
        size_t uintObservations = 0;
        size_t doubleObservations = 0;
        size_t boolObservations = 0;
        bool killed = false;

        EnumCandidateTracker() = default;
        EnumCandidateTracker(uint16_t capValue, uint32_t valueMaxLenValue) noexcept
            : valueMaxLen(valueMaxLenValue), cap(capValue)
        {
            values.reserve(capValue);
            seen.reserve(capValue);
        }

        /// Caller has already incremented `presenceCount`. Updates
        /// state and flips `killed` on tolerance breach or hard-cap
        /// overflow.
        void Observe(std::string_view bytes);
    };
    // NOLINTEND(misc-non-private-member-variables-in-classes)

    /// Cumulative health for an active enum column; long values and
    /// wrong-type slots share one budget.
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    // Private nested aggregate POD: public data members are intentional.
    struct EnumColumnHealth
    {
        size_t totalSlots = 0;
        size_t longValueSlots = 0;
        size_t wrongTypeSlots = 0;

        [[nodiscard]] bool ShouldDemote(double tolerance, size_t minSamples) const noexcept;
    };
    // NOLINTEND(misc-non-private-member-variables-in-classes)

    static std::string FormatLogValue(const std::string &format, const LogValue &value);

    void RefreshColumnKeyIds();
    void RefreshColumnKeyIdsForKeys(const std::vector<std::string> &newKeys);
    void RefreshSnapshotTimeKeys();
    void RefreshSnapshotEnumKeys();

    /// Point every owned `LineSource` at `mEnumDictionaries`.
    void RewireSourceRegistries();

    /// Enum pass over `[oldLineCount, Lines().size())`: encode active
    /// columns, demote overflowing ones, auto-promote quiescent
    /// candidates. Extends @p firstBackfilled / @p lastBackfilled.
    void RunEnumPassForAppendBatch(
        size_t oldLineCount, std::optional<size_t> &firstBackfilled, std::optional<size_t> &lastBackfilled
    );

    /// Promote @p columnIndex to `Type::Enumeration`, encoding every
    /// existing row's slot as `DictRef`.
    void PromoteColumnToEnum(size_t columnIndex);

    /// If @p columnIndex is `Type::Enumeration` and (a) its key matches
    /// `IsLogLevelKey` and (b) at least 80% of the observed rows for the
    /// column resolve via `ResolveLevel`, flip the type to `Type::Level`
    /// and populate the cache entry in `mLevelRankCache`. Counting by
    /// row occurrence (not distinct dictionary entries) keeps a flood of
    /// canonical rows from being defeated by one rare non-canonical
    /// value. No-op otherwise.
    void MaybePromoteToLevel(size_t columnIndex);

    /// Rebuild / extend the cached `EnumValueId -> LogLevel` table for the
    /// column at @p columnIndex. Idempotent; safe to call after dictionary
    /// growth. No-op if the column is not `Type::Level`.
    void RefreshLevelRankCache(size_t columnIndex);

    /// Demote @p columnIndex from enum to string, materialising every
    /// `DictRef` into `OwnedString` and dropping the dictionary.
    void DemoteColumnFromEnum(size_t columnIndex);

    /// Encode column slots in `[rowBegin, rowEnd)` as `DictRef`. Returns
    /// false on hard cap overflow; long/wrong-type slots accrue in @p health.
    bool EncodeColumnRangeAsEnum(
        const LogConfiguration::Column &column, size_t rowBegin, size_t rowEnd, EnumColumnHealth &health
    );

    /// Shared encode loop. @p aliasKeys[0] is the canonical dictionary key.
    /// Returns false on hard cap overflow.
    bool EncodeColumnRange(std::span<const KeyId> aliasKeys, size_t rowBegin, size_t rowEnd, EnumColumnHealth &health);

    LogData mData;
    LogConfigurationManager mConfiguration;
    std::vector<std::vector<KeyId>> mColumnKeyIds;

    /// `Type::Time` KeyIds present at `BeginStreaming`; Stage B promotes inline.
    std::unordered_set<KeyId> mStageBSnapshotTimeKeys;

    /// `Type::Time` KeyIds discovered post-snapshot.
    std::unordered_set<KeyId> mPostSnapshotTimeKeys;

    /// Per-column enum dictionaries. Owned `LineSource`s point here.
    EnumDictionaryRegistry mEnumDictionaries;

    uint16_t mEnumValueCap = DEFAULT_ENUM_VALUE_CAP;

    /// Per-value byte-length cap (`0` disables).
    uint32_t mEnumValueMaxLen = MAX_ENUM_CANDIDATE_LEN;

    /// Promotion candidates keyed on `column.header`; live while
    /// `Type::Unknown`.
    std::unordered_map<std::string, EnumCandidateTracker> mEnumTrackers;

    /// Cumulative health for active enum columns, keyed on `column.header`.
    std::unordered_map<std::string, EnumColumnHealth> mEnumColumnHealth;

    /// True between `BeginStreaming` and `FinalizeAutoDetection`; switches
    /// to stream-mode thresholds and disables the cardinality bail.
    bool mIsStreaming = false;

    std::optional<std::pair<size_t, size_t>> mLastBackfillRange;

    /// Canonical KeyIds demoted away from `Type::Enumeration` during
    /// the in-progress (or most recent) batch. Populated by
    /// `DemoteColumnFromEnum` *before* it erases the registry entry
    /// so the id stays stable; consumed by `LogModel` to scope its
    /// `enumColumnsChanged(Demoted)` emit.
    std::vector<KeyId> mLastBatchDemotedKeys;

    /// Per-`Type::Level`-column `EnumValueId -> LogLevel` cache. Keyed
    /// by `Column::header` (same idiom `mEnumColumnHealth` uses) so
    /// column reorder is automatic. Empty entries for non-Level columns
    /// are filtered at `RefreshLevelRankCache`.
    std::unordered_map<std::string, std::vector<LogLevel>> mLevelRankCache;
};

} // namespace loglib
