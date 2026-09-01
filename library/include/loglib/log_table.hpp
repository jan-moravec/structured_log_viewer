#pragma once

#include "column_type_health.hpp"
#include "enum_dictionary.hpp"
#include "enum_inference.hpp"
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
#include <unordered_set>
#include <utility>
#include <vector>

namespace loglib
{

/**
 * @brief Row-range facade backed by `std::vector<LogLine>` for static and
 * live-tail sessions. Each row's `LineSource *` resolves its values.
 *
 * Independently changing work lives on named collaborators:
 * `EnumInference` (type inference, dictionary encode/demote, level
 * rank cache; owns those maps and borrows `LogData` /
 * `LogConfigurationManager` per pass), `ColumnTypeHealth` /
 * `ComputeColumnTypeHealth` (stateless diagnostics), and
 * `internal::RefreshColumnKeyIds` (stateless cache rewrite).
 * Streaming batch splice, timestamp snapshot keys, and FIFO
 * eviction stay here because they own the row storage.
 */
class LogTable
{
public:
    LogTable() = default;

    LogTable(LogData data, LogConfigurationManager configuration);

    LogTable(LogTable &) = delete;
    LogTable &operator=(const LogTable &) = delete;

    /**
     * @brief Move re-runs `RewireSourceRegistries()` because each `LineSource`
     * caches a pointer to `mEnum.Dictionaries()`. Per-batch bookkeeping
     * (notably `LastBatchDemotedKeys()`) follows the move so a table
     * moved between `AppendBatch` and the `LogModel`-side consumer
     * keeps a faithful `Demoted`-reason trail.
     */
    LogTable(LogTable &&) noexcept;
    LogTable &operator=(LogTable &&) noexcept;

    /** @brief Replaces the table's data with a freshly-merged @p data. */
    void Update(LogData &&data);

    /** @brief Clears `Data()` and streaming-time-key snapshots; preserves `Configuration()`. */
    void Reset();

    /**
     * @brief Initialise for a streaming parse and snapshot time-column KeyIds.
     * `Configuration()` must not mutate until streaming finishes.
     * Takes ownership of @p source (may be null).
     */
    void BeginStreaming(std::unique_ptr<LineSource> source);

    /** @brief Multi-file streaming append; adds @p source without resetting. */
    void AppendStreaming(std::unique_ptr<LineSource> source);

    /**
     * @brief Splice @p batch in, extending the configuration for any
     * `batch.newKeys` and back-filling new `Type::Time` columns.
     */
    void AppendBatch(StreamedBatch batch);

    /**
     * @brief Non-mutating preview of `AppendBatch(batch)`'s effect; lets the
     * model call `beginInsert{Rows,Columns}` first.
     */
    struct AppendBatchPreview
    {
        size_t newRowCount = 0;
        size_t newColumnCount = 0;
    };
    [[nodiscard]] AppendBatchPreview PreviewAppend(const StreamedBatch &batch) const;

    /**
     * @brief Inclusive `[firstColumn, lastColumn]` back-filled by the last
     * `AppendBatch`, or nullopt.
     */
    [[nodiscard]] const std::optional<std::pair<size_t, size_t>> &LastBackfillRange() const noexcept;

    /**
     * @brief Canonical `KeyId`s of columns demoted away from
     * `Type::Enumeration` during the most recent `AppendBatch` /
     * `Update` / `BeginStreaming`. Includes the silent
     * "promoted-and-demoted-in-the-same-batch" case
     * (`Type::Any + autoDetect -> Enumeration -> String`) where the
     * registry shows no dict before or after, which the `LogModel`-
     * side `enumDictSizesBefore` snapshot can't see. Reset at the
     * start of every batch-style call. Empty when no column demoted.
     */
    [[nodiscard]] const std::vector<KeyId> &LastBatchDemotedKeys() const noexcept;

    /**
     * @brief Reorder column @p srcIndex to @p destIndex. Callers must wrap with
     * `beginMoveColumns`/`endMoveColumns`.
     */
    void MoveColumn(size_t srcIndex, size_t destIndex);

    /**
     * @brief Canonical `KeyId`s of columns that `MaybePromoteToLevel`
     * flipped to `Type::Level` since the last drain. The bubble
     * to `CANONICAL_LEVEL_COLUMN_INDEX` is deferred so the
     * streaming consumer (`LogModel`) can wrap each move in
     * `begin/endMoveColumns`. Caller must re-resolve each
     * `KeyId` to its current column index before moving --
     * earlier moves can shift later targets.
     *
     * Static-load paths use `ApplyPendingLevelBubbles` instead
     * and leave this queue empty.
     */
    [[nodiscard]] std::vector<KeyId> TakePendingLevelBubbleKeys() noexcept;

    /**
     * @brief Drain the pending-bubble queue and apply each via
     * `MoveColumn`. No Qt-side notifications -- intended for
     * static-load paths whose callers reset the model afterward.
     */
    void ApplyPendingLevelBubbles();

    /**
     * @brief First column whose key list maps to @p kid, or `-1`. Linear
     * in (columns * keys-per-column); fine for per-batch use
     * since column counts are tens at most.
     */
    [[nodiscard]] int FindColumnIndexByKey(KeyId kid) const noexcept;

    /** @brief Pre-allocation hint forwarded to the streaming `LogFile`. */
    void ReserveLineOffsets(size_t count);

    [[nodiscard]] std::string GetHeader(size_t column) const;
    [[nodiscard]] size_t ColumnCount() const;
    [[nodiscard]] LogValue GetValue(size_t row, size_t column) const;
    [[nodiscard]] std::string GetFormattedValue(size_t row, size_t column) const;

    /**
     * @brief One-walk variant for string-predicate consumers. Resolves the
     * slot at (@p row, @p column) once and returns its bytes:
     *   - `string_view` slots (mmap-aliased, dictionary-resolved)
     *     are returned directly. Valid as long as the underlying
     *     line / source / dictionary lives.
     *   - `std::string` slots and non-string slots (numeric, time,
     *     bool) are written into @p buffer (via the column's
     *     `printFormat` for numeric / time) and the view aliases
     *     @p buffer.
     *   - Absent slots return an empty view.
     *
     * Callers own @p buffer for the lifetime of the returned view.
     * The previous `GetValue` + `GetFormattedValue` pair walked the
     * line twice for every non-string column hit; this collapses the
     * predicate hot path to one walk.
     */
    [[nodiscard]] std::string_view GetValueOrFormatted(size_t row, size_t column, std::string &buffer) const;

    [[nodiscard]] size_t RowCount() const;

    /**
     * @brief Borrowed row/source storage. Mutation is restricted to splice,
     * merge, and eviction paths (`AppendBatch`, `Update`, `Reset`,
     * `EvictPrefixRows`) plus tests that assemble fixtures. Callers
     * must not insert rows that skip `AppendBatch`'s timestamp
     * back-fill and enum pass.
     */
    [[nodiscard]] const LogData &Data() const noexcept;
    [[nodiscard]] LogData &Data() noexcept;

    /**
     * @brief Drop the first @p count rows; callers wrap with
     * `beginRemoveRows`/`endRemoveRows`.
     */
    void EvictPrefixRows(size_t count);

    /**
     * @brief Session `KeyIndex`. The mutable overload exists so parse
     * workers can intern keys before `AppendBatch`; it is the same
     * intern table `Data().Keys()` returns. Do not replace or clear
     * it independently of the rows that cite those `KeyId`s.
     */
    KeyIndex &Keys();
    const KeyIndex &Keys() const;

    /** @brief Per-column enum dictionaries. Filter UI reads `Find(keyId)->Values()`. */
    const EnumDictionaryRegistry &EnumDictionaries() const noexcept;

    /**
     * @brief Test/tuning: override the per-column distinct-value cap. Clamped to
     * `[1, MAX_ENUM_VALUES]`. No effect on existing dictionaries.
     */
    void SetEnumValueCap(uint16_t cap) noexcept;

    [[nodiscard]] uint16_t EnumValueCap() const noexcept;

    /**
     * @brief Test/tuning: override the per-value byte-length cap (`0` disables).
     * Long values accrue against the column's health budget.
     */
    void SetEnumValueMaxLen(uint32_t maxLen) noexcept;

    [[nodiscard]] uint32_t EnumValueMaxLen() const noexcept;

    /**
     * @brief Dictionary id for the slot at @p row, @p column when it's a `DictRef`,
     * else nullopt. Powers the `EnumValueRole` fast-filter path.
     */
    [[nodiscard]] std::optional<EnumValueId> GetEnumValueId(size_t row, size_t column) const noexcept;

    /**
     * @brief Canonical level for the slot at (@p row, @p columnIndex) on a
     * `Type::Level` column. Returns `std::nullopt` for non-Level
     * columns, monostate slots, or unmapped dictionary entries.
     */
    [[nodiscard]] std::optional<LogLevel> GetLevelForRow(size_t row, size_t columnIndex) const noexcept;

    /**
     * @brief Like `GetLevelForRow` but returns `LogLevel::Unknown` for
     * values that exist but didn't resolve to a canonical level,
     * so the icon-mode paint path can render a generic "unknown"
     * glyph. Also surfaces `Unknown` for the rare streaming-only
     * race where the rank cache hasn't grown to include a
     * just-interned dictionary id. `std::nullopt` still means
     * "no value at all". Sort / styling paths intentionally keep
     * using `GetLevelForRow` so unmapped values stay unstyled.
     */
    [[nodiscard]] std::optional<LogLevel> GetDisplayLevelForRow(size_t row, size_t columnIndex) const noexcept;

    /**
     * @brief `EnumValueId -> LogLevel` cache for a `Type::Level` column.
     * `ranks[id]` is the canonical level for dictionary entry `id`, or
     * `LogLevel::Unknown` if the raw string did not resolve. Returns
     * `nullptr` when the column is not `Type::Level`, is out of range,
     * or its canonical key has not been observed yet.
     *
     * Keyed internally by canonical `KeyId` (not `column.header`), so
     * two Level columns whose headers collide keep separate caches.
     */
    [[nodiscard]] const std::vector<LogLevel> *LevelRankCache(size_t columnIndex) const noexcept;

    /**
     * @brief Outcome of `ResolveEnumColumn`:
     *   - `canonicalKey == INVALID_KEY_ID`: column out of range, has
     *     no keys, or its first key isn't interned. Skip enum logic.
     *   - `canonicalKey` valid, `dictionary == nullptr`: column has a
     *     canonical key but is not currently `Type::Enumeration`.
     *     Predicates fall back to the string-set path.
     *   - Both populated: column is promoted; caller can use the
     *     dictionary directly.
     */
    struct EnumColumnLookup
    {
        KeyId canonicalKey = INVALID_KEY_ID;
        const EnumDictionary *dictionary = nullptr;
    };

    /**
     * @brief Resolve `column index -> canonical KeyId -> EnumDictionary*`.
     * Returns a default-constructed lookup on any miss.
     */
    [[nodiscard]] EnumColumnLookup ResolveEnumColumn(size_t columnIndex) const noexcept;

    /**
     * @brief End-of-parse/end-of-stream auto-detection sweep: promote
     * permissive candidates and transition leftover `Type::Any +
     * autoDetect` columns to a terminal type (or leave them as
     * `Type::Any + autoDetect` for a future re-load when there is
     * insufficient evidence). Idempotent. Returns true if at least
     * one column was promoted to `Type::Enumeration`.
     */
    bool FinalizeAutoDetection();

    /**
     * @brief Reconcile loaded rows with a user-driven type flip at
     * @p columnIndex. The caller (`ColumnEditor::Apply`) has already
     * written the new `(type, autoDetect)` into the configuration;
     * this method back-fills existing rows so the change applies
     * immediately rather than waiting for the next batch (which a
     * fully-loaded static file never gets):
     *   - `Time`: seeds default formats if missing, runs
     *     `BackfillTimestampColumn` over every row.
     *   - `Enumeration` / `Level`: creates the dictionary, encodes
     *     every slot as `DictRef`, refreshes the level rank cache.
     *   - Anything else: tears down the dictionary / rank cache,
     *     materialises `DictRef` slots back to owned strings
     *     (same effect as `DemoteColumnFromEnum`). When
     *     @p previousType is `Time`, also clears `printFormat` /
     *     `parseFormats` so a stale strftime string does not leak
     *     into the new rendering path.
     * @p previousType is used for the Time-format reset and to
     * short-circuit no-op transitions (e.g. an
     * `Enumeration -> Enumeration` autoDetect toggle preserves the
     * accumulated health budget). Idempotent within a type.
     * Out-of-range @p columnIndex is a silent no-op.
     */
    void OnUserChangedColumnType(size_t columnIndex, ColumnType previousType);

    /**
     * @brief Re-sync per-column caches with `mConfiguration` after an
     * out-of-band rewrite (the GUI's `LogConfigurationManager::Load`
     * path). `Reset()` only refreshes against the pre-load
     * configuration; this runs after the new columns have landed.
     * Safe to call on an empty table.
     */
    void OnConfigurationReloaded();

    /**
     * @brief Re-run the auto-detector over every existing row of
     * @p columnIndex as if the column had just been freshly streamed.
     * Used by the Column Editor when the user flips a static-file
     * column to `(Any, autoDetect)`, so the pick takes effect
     * immediately rather than waiting for a never-arriving batch.
     *
     * No-op when the column is not currently `(Any, autoDetect)`,
     * the table is empty, or @p columnIndex is out of range.
     * Returns the post-rescan column type for transition signalling.
     */
    ColumnType RescanColumnForAutoDetection(size_t columnIndex);

    /** @brief Nested alias so existing `LogTable::ColumnTypeHealth` spellings compile. */
    using ColumnTypeHealth = loglib::ColumnTypeHealth;

    /**
     * @brief "Does this column's data match its configured `Type`?"
     * Delegates to the stateless `loglib::ComputeColumnTypeHealth`.
     */
    [[nodiscard]] ColumnTypeHealth ComputeColumnTypeHealth(size_t columnIndex) const;

    const LogConfigurationManager &Configuration() const;
    /** @brief Non-const access for `Load`/`Save`. Must not be mutated mid-streaming. */
    LogConfigurationManager &Configuration();

private:
    static std::string FormatLogValue(const std::string &format, const LogValue &value);

    void RefreshColumnKeyIds();
    void RefreshColumnKeyIdsForKeys(const std::vector<std::string> &newKeys);
    void RefreshSnapshotTimeKeys();

    /** @brief Point every owned `LineSource` at `mEnum.Dictionaries()`. */
    void RewireSourceRegistries();

    LogData mData;
    LogConfigurationManager mConfiguration;
    std::vector<std::vector<KeyId>> mColumnKeyIds;

    /** @brief `Type::Time` KeyIds present at `BeginStreaming`; Stage B promotes inline. */
    std::unordered_set<KeyId> mStageBSnapshotTimeKeys;

    /** @brief `Type::Time` KeyIds discovered post-snapshot. */
    std::unordered_set<KeyId> mPostSnapshotTimeKeys;

    /**
     * @brief Type inference, dictionary encode/demote, and level rank cache.
     * Owns dictionaries; `LineSource`s borrow `Dictionaries()`.
     */
    EnumInference mEnum;

    std::optional<std::pair<size_t, size_t>> mLastBackfillRange;
};

} // namespace loglib
