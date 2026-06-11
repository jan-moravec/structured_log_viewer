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
    /// (`Type::Any + autoDetect -> Enumeration -> String`) where the
    /// registry shows no dict before or after, which the `LogModel`-
    /// side `enumDictSizesBefore` snapshot can't see. Reset at the
    /// start of every batch-style call. Empty when no column demoted.
    [[nodiscard]] const std::vector<KeyId> &LastBatchDemotedKeys() const noexcept;

    /// Reorder column @p srcIndex to @p destIndex. Callers must wrap with
    /// `beginMoveColumns`/`endMoveColumns`.
    void MoveColumn(size_t srcIndex, size_t destIndex);

    /// Canonical `KeyId`s of columns that `MaybePromoteToLevel`
    /// flipped to `Type::Level` since the last drain, but whose
    /// canonical-position bubble has been deferred so the
    /// streaming consumer (`LogModel`) can wrap each follow-up
    /// `MoveColumn` in `begin/endMoveColumns`. Returned in
    /// queue order; the caller is responsible for resolving each
    /// `KeyId` to its current column index (via
    /// `Configuration().columns[i].keys`) immediately before its
    /// move, because an earlier move can shift later targets.
    ///
    /// Static-load paths (`LogTable` ctor + `Update`) call
    /// `ApplyPendingLevelBubbles` instead and never read this
    /// queue; the queue is therefore always empty when those
    /// paths return. Tests that exercise `AppendBatch` /
    /// `FinalizeAutoDetection` directly can either call
    /// `ApplyPendingLevelBubbles` to mimic the static-load path,
    /// or drain and apply the queue manually to mimic the
    /// streaming consumer.
    [[nodiscard]] std::vector<KeyId> TakePendingLevelBubbleKeys() noexcept;

    /// Drain `mPendingLevelBubbleKeys` and apply each bubble via
    /// `MoveColumn`. No Qt-side notifications -- intended for
    /// static load paths whose callers follow up with a model
    /// reset (and for tests that don't exercise the
    /// `LogModel`-side streaming consumer).
    void ApplyPendingLevelBubbles();

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

    /// Canonical level for the slot at (@p row, @p columnIndex) on a
    /// `Type::Level` column. Returns `std::nullopt` for non-Level
    /// columns, monostate slots, or unmapped dictionary entries.
    [[nodiscard]] std::optional<LogLevel> GetLevelForRow(size_t row, size_t columnIndex) const noexcept;

    /// `EnumValueId -> LogLevel` cache for a `Type::Level` column.
    /// `ranks[id]` is the canonical level for dictionary entry `id`, or
    /// `LogLevel::Unknown` if the raw string did not resolve. Returns
    /// `nullptr` when the column is not `Type::Level`, is out of range,
    /// or its canonical key has not been observed yet.
    ///
    /// Keyed internally by canonical `KeyId` (not `column.header`), so
    /// two Level columns whose headers collide keep separate caches.
    [[nodiscard]] const std::vector<LogLevel> *LevelRankCache(size_t columnIndex) const noexcept;

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

    /// End-of-parse/end-of-stream auto-detection sweep: promote
    /// permissive candidates and transition leftover `Type::Any +
    /// autoDetect` columns to a terminal type (or leave them as
    /// `Type::Any + autoDetect` for a future re-load when there is
    /// insufficient evidence). Idempotent. Returns true if at least
    /// one column was promoted to `Type::Enumeration`.
    bool FinalizeAutoDetection();

    /// Reconcile loaded rows with a user-driven type flip at
    /// @p columnIndex. The caller (`ColumnEditor::Apply`) has already
    /// written the new `(type, autoDetect)` into the configuration;
    /// this method back-fills existing rows so the change applies
    /// immediately rather than waiting for the next batch (which a
    /// fully-loaded static file never gets):
    ///   - `Time`: seeds default formats if missing, runs
    ///     `BackfillTimestampColumn` over every row.
    ///   - `Enumeration` / `Level`: creates the dictionary, encodes
    ///     every slot as `DictRef`, refreshes the level rank cache.
    ///   - Anything else: tears down the dictionary / rank cache,
    ///     materialises `DictRef` slots back to owned strings
    ///     (same effect as `DemoteColumnFromEnum`). When
    ///     @p previousType is `Time`, also clears `printFormat` /
    ///     `parseFormats` so a stale strftime string does not leak
    ///     into the new rendering path.
    /// @p previousType is used for the Time-format reset and to
    /// short-circuit no-op transitions (e.g. an
    /// `Enumeration -> Enumeration` autoDetect toggle preserves the
    /// accumulated health budget). Idempotent within a type.
    /// Out-of-range @p columnIndex is a silent no-op.
    void OnUserChangedColumnType(size_t columnIndex, LogConfiguration::Type previousType);

    /// Re-sync per-column caches with `mConfiguration` after an
    /// out-of-band rewrite (the GUI's `LogConfigurationManager::Load`
    /// path). `Reset()` only refreshes against the pre-load
    /// configuration; this runs after the new columns have landed.
    /// Safe to call on an empty table.
    void OnConfigurationReloaded();

    /// Re-run the auto-detector over every existing row of
    /// @p columnIndex as if the column had just been freshly streamed.
    /// Used by the Column Editor when the user flips a static-file
    /// column to `(Any, autoDetect)`, so the pick takes effect
    /// immediately rather than waiting for a never-arriving batch.
    ///
    /// No-op when the column is not currently `(Any, autoDetect)`,
    /// the table is empty, or @p columnIndex is out of range.
    /// Returns the post-rescan column type for transition signalling.
    LogConfiguration::Type RescanColumnForAutoDetection(size_t columnIndex);

    /// "Does this column's data match its configured `Type`?"
    /// Computed on demand for the diagnostics UI; one column-walk
    /// per call, no hot-path bookkeeping.
    struct ColumnTypeHealth
    {
        // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
        /// Total rows in the table.
        size_t totalSlots = 0;
        /// Rows where this column carries any (non-monostate) value.
        size_t presentSlots = 0;
        /// Present slots whose variant matches the configured `Type`.
        /// `Type::Any` matches every present slot. For
        /// `Enumeration` / `Level`, `DictRef` slots match; unencoded
        /// raw-string slots count as present-but-not-matching (this
        /// is how user-pinned dict columns expose over-cap values).
        size_t matchingSlots = 0;
        // NOLINTEND(misc-non-private-member-variables-in-classes)

        [[nodiscard]] constexpr bool operator==(const ColumnTypeHealth &) const = default;
    };
    [[nodiscard]] ColumnTypeHealth ComputeColumnTypeHealth(size_t columnIndex) const;

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

    /// Promote @p columnIndex from `Type::Enumeration` to `Type::Level`
    /// when (a) its key matches `IsLogLevelKey` and (b) the dictionary
    /// has at most one unrecognized entry per `LEVEL_DICT_TOLERANCE_RATIO`
    /// canonical ones. Dict-weighted, so re-evaluation only matters
    /// when the dictionary grows. No-op otherwise. `O(dict size)`.
    ///
    /// Does not perform the canonical-position bubble inline.
    /// Instead the canonical `KeyId` is queued on
    /// `mPendingLevelBubbleKeys` so the streaming consumer
    /// (`LogModel`) can wrap each follow-up `MoveColumn` in
    /// `begin/endMoveColumns`. Static-load paths drain the queue
    /// via `ApplyPendingLevelBubbles`.
    void MaybePromoteToLevel(size_t columnIndex);

    /// Rebuild / extend the `EnumValueId -> LogLevel` cache. Idempotent;
    /// safe to call after dictionary growth. No-op for non-Level columns.
    void RefreshLevelRankCache(size_t columnIndex);

    /// Demote @p columnIndex to `Type::String`, materialising every
    /// `DictRef` into `OwnedString` and dropping the dictionary. Also
    /// handles the `Type::Level -> Type::String` path: a Level column
    /// that breaches the enum health budget drops straight to terminal
    /// String (kill-once-stay-killed -- bouncing back to Enumeration
    /// would just cycle into another demote). The level rank cache is
    /// torn down here so no stale metadata trails the column.
    ///
    /// @p recordForBatch (default `true`): record the demoted column
    /// in `mLastBatchDemotedKeys` for the streaming auto-detect
    /// path. The editor path passes `false` because it emits its own
    /// `enumColumnsChanged(Demoted)` signal -- adding to the batch
    /// vector would double-signal.
    void DemoteColumnFromEnum(size_t columnIndex, bool recordForBatch = true);

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

    /// Promotion candidates, keyed by canonical `KeyId` of the
    /// column. Keying by id (not `column.header`) so a user-driven
    /// header rename cannot orphan the running tracker and reset
    /// the budget. Live while the column is `(Any, autoDetect)`.
    std::unordered_map<KeyId, EnumCandidateTracker> mEnumTrackers;

    /// Cumulative health for active enum columns, keyed by canonical
    /// `KeyId` for the same rename-safety reason as `mEnumTrackers`.
    std::unordered_map<KeyId, EnumColumnHealth> mEnumColumnHealth;

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

    /// `EnumValueId -> LogLevel` cache, one entry per `Type::Level`
    /// column. Keyed by canonical `KeyId` (matches the dictionary
    /// registry), so column reorders are automatic and same-header
    /// columns with different keys cannot alias each other.
    std::unordered_map<KeyId, std::vector<LogLevel>> mLevelRankCache;

    /// Queue of canonical `KeyId`s whose columns were freshly
    /// promoted to `Type::Level` since the last drain, but whose
    /// canonical-position bubble has been deferred. See
    /// `MaybePromoteToLevel` and `TakePendingLevelBubbleKeys`.
    std::vector<KeyId> mPendingLevelBubbleKeys;
};

} // namespace loglib
