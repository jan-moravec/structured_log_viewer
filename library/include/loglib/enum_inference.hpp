#pragma once

#include "loglib/column_schema.hpp"
#include "loglib/enum_dictionary.hpp"
#include "loglib/key_index.hpp"
#include "loglib/log_level.hpp"
#include "loglib/transparent_string_hash.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace loglib
{

class LogData;
struct LogConfiguration;
class LogConfigurationManager;

/// Static-parse min rows before promoting `Type::Any + autoDetect` to enum.
/// Smaller files are caught at end-of-parse by `EnumInference::Finalize`.
inline constexpr size_t ENUM_PROMOTION_MIN_ROWS = 4096;

/// Streaming promotion threshold; promote eagerly, dictionary/length caps guard.
inline constexpr size_t STREAM_PROMOTION_MIN_ROWS = 2;

/// Static-mode cardinality bail: distinct/observed ratio limit.
inline constexpr double ENUM_CARDINALITY_BAIL_RATIO = 0.05;

/// Max fraction of over-cap-length or wrong-type observations before demotion.
inline constexpr double ENUM_HEALTH_TOLERANCE_RATIO = 0.01;

/// Minimum samples before consulting the tolerance ratio.
inline constexpr size_t ENUM_HEALTH_MIN_SAMPLES = 50;

/// Dict-weighted tolerance for `MaybePromoteToLevel`.
inline constexpr size_t LEVEL_DICT_TOLERANCE_RATIO = 4;

/**
 * @brief Per-column tracker for enum auto-detection.
 *
 * Holds up to `cap` distinct values (hard cap, no tolerance). Long
 * values accrue in `longValueCount`; numeric-tag counters route the
 * no-string bail to a numeric type rather than `string`.
 * `presenceCount` and `rowsObserved` are separate so sparse columns
 * aren't bailed before their first observation.
 *
 * Owned by `EnumInference`. Per-row `Observe` is a concrete method
 * (no virtual dispatch).
 */
struct EnumCandidateTracker
{
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    /** @brief Distinct values seen so far (insertion order, capped at `cap`). */
    std::vector<std::string> values;
    /**
     * @brief O(1) membership index over `values`. Transparent hashing
     * avoids the per-row `std::string` materialisation a
     * non-transparent `unordered_set<string>` would force on
     * every `string_view` lookup.
     */
    std::unordered_set<std::string, TransparentStringHash, TransparentStringEqual> seen;
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
    // NOLINTEND(misc-non-private-member-variables-in-classes)

    EnumCandidateTracker() = default;
    EnumCandidateTracker(uint16_t capValue, uint32_t valueMaxLenValue) noexcept
        : valueMaxLen(valueMaxLenValue), cap(capValue)
    {
        values.reserve(capValue);
        seen.reserve(capValue);
    }

    /**
     * @brief Caller has already incremented `presenceCount`. Updates
     * state and flips `killed` on tolerance breach or hard-cap
     * overflow.
     */
    void Observe(std::string_view bytes);
};

/**
 * @brief Cumulative health for an active enum column; long values and
 * wrong-type slots share one budget.
 */
struct EnumColumnHealth
{
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    size_t totalSlots = 0;
    size_t longValueSlots = 0;
    size_t wrongTypeSlots = 0;
    // NOLINTEND(misc-non-private-member-variables-in-classes)

    [[nodiscard]] bool ShouldDemote(double tolerance, size_t minSamples) const noexcept;
};

/**
 * @brief Pick a terminal type from observed numeric / bool tag counts.
 *
 *   - bools only         -> `Boolean`
 *   - integers / doubles -> `Integer` / `Floating` / `Number`
 *   - bool + numeric mix -> `Any` (no specialised widget)
 *   - nothing observed   -> `Any` (historical bail)
 *
 * An already-promoted enum that later sees bool slots demotes to
 * `Type::String` (via lumped `wrongTypeSlots`), not `Boolean`.
 */
[[nodiscard]] ColumnType RouteNoStringBail(
    size_t intObservations, size_t uintObservations, size_t doubleObservations, size_t boolObservations
) noexcept;

/**
 * @brief Returns whether @p column participates in the enum encode / candidate
 * scan. `Enumeration` / `Level` always do; `Any` only when `autoDetect`.
 */
[[nodiscard]] bool IsEnumPassEligible(const Column &column) noexcept;

/**
 * @brief Type-inference and enum/level cache collaborator for `LogTable`.
 *
 * Owns dictionaries, candidate trackers, health budgets, the level
 * rank cache, and pending level-bubble keys. Borrows `LogData` and
 * `LogConfigurationManager` for each mutating pass; does not own
 * row storage. Callers construct one instance per table.
 *
 * Hot paths (`Observe`, `EncodeColumnRange`) are concrete member
 * functions. There is no per-row virtual dispatch and no extra
 * heap allocation beyond the dictionaries and trackers the table
 * already required.
 */
class EnumInference
{
public:
    EnumInference() = default;

    EnumInference(const EnumInference &) = delete;
    EnumInference &operator=(const EnumInference &) = delete;

    /**
     * @brief Move rebinds no source registries; `LogTable` calls
     * `RewireSourceRegistries` after moving because each
     * `LineSource` caches a pointer to `Dictionaries()`.
     */
    EnumInference(EnumInference &&) noexcept = default;
    EnumInference &operator=(EnumInference &&) noexcept = default;

    ~EnumInference() = default;

    /** @brief Drops dictionaries, trackers, health, rank cache, and batch trails. */
    void Clear();

    /**
     * @brief Clears inference caches and enables stream-mode thresholds
     * (`STREAM_PROMOTION_MIN_ROWS`, no cardinality bail).
     */
    void BeginStreaming();

    void SetStreaming(bool streaming) noexcept;
    [[nodiscard]] bool IsStreaming() const noexcept;

    void SetValueCap(uint16_t cap) noexcept;
    [[nodiscard]] uint16_t ValueCap() const noexcept;
    void SetValueMaxLen(uint32_t maxLen) noexcept;
    [[nodiscard]] uint32_t ValueMaxLen() const noexcept;

    [[nodiscard]] EnumDictionaryRegistry &Dictionaries() noexcept;
    [[nodiscard]] const EnumDictionaryRegistry &Dictionaries() const noexcept;

    [[nodiscard]] const std::vector<KeyId> &LastBatchDemotedKeys() const noexcept;
    void ClearLastBatchDemotedKeys() noexcept;

    [[nodiscard]] std::vector<KeyId> TakePendingLevelBubbleKeys() noexcept;

    void EraseTracker(KeyId canonical);
    [[nodiscard]] EnumColumnHealth &HealthFor(KeyId canonical);
    void EraseHealthAndLevelCache(KeyId canonical);

    /**
     * @brief Pre-create dictionaries for configured enum / level columns
     * and seed the level rank cache. Idempotent. Clears the rank
     * cache first so a freshly-loaded `levelMapping` is honoured.
     */
    void RefreshSnapshot(LogData &data, const LogConfigurationManager &configuration);

    /**
     * @brief Enum pass over `[oldLineCount, Lines().size())`: encode active
     * columns, demote overflowing ones, auto-promote quiescent
     * candidates. Extends @p firstBackfilled / @p lastBackfilled.
     */
    void RunPassForAppendBatch(
        LogData &data,
        LogConfigurationManager &configuration,
        const std::vector<std::vector<KeyId>> &columnKeyIds,
        size_t oldLineCount,
        std::optional<size_t> &firstBackfilled,
        std::optional<size_t> &lastBackfilled
    );

    /**
     * @brief End-of-parse/end-of-stream auto-detection sweep. Idempotent.
     * Returns true if at least one column was promoted to
     * `Type::Enumeration`.
     */
    bool Finalize(LogData &data, LogConfigurationManager &configuration);

    /**
     * @brief Re-run the detector over every existing row of @p columnIndex.
     * Returns the post-rescan column type.
     */
    ColumnType RescanColumn(
        LogData &data,
        LogConfigurationManager &configuration,
        const std::vector<std::vector<KeyId>> &columnKeyIds,
        size_t columnIndex
    );

    /**
     * @brief Promote @p columnIndex to `Type::Enumeration`, encoding every
     * existing row's slot as `DictRef`.
     */
    void PromoteColumnToEnum(LogData &data, LogConfigurationManager &configuration, size_t columnIndex);

    /**
     * @brief Demote @p columnIndex to `Type::String`, materialising every
     * `DictRef` into `OwnedString` and dropping the dictionary.
     *
     * @p recordForBatch records the demoted column in
     * `LastBatchDemotedKeys`. The editor path passes `false`.
     */
    void DemoteColumnFromEnum(
        LogData &data, LogConfigurationManager &configuration, size_t columnIndex, bool recordForBatch = true
    );

    /**
     * @brief Promote @p columnIndex from `Type::Enumeration` to `Type::Level`
     * when the key matches `IsLogLevelKey` and the dictionary
     * satisfies `LEVEL_DICT_TOLERANCE_RATIO`. Queues a bubble
     * `KeyId`; does not move columns.
     */
    void MaybePromoteToLevel(LogData &data, LogConfigurationManager &configuration, size_t columnIndex);

    /**
     * @brief Rebuild / extend the `EnumValueId -> LogLevel` cache. No-op
     * for non-Level columns.
     */
    void RefreshLevelRankCache(const LogData &data, const LogConfiguration &configuration, size_t columnIndex);

    /**
     * @brief Encode column slots in `[rowBegin, rowEnd)` as `DictRef`.
     * Returns false on hard cap overflow; long/wrong-type slots
     * accrue in @p health.
     */
    bool EncodeColumnRange(
        LogData &data, std::span<const KeyId> aliasKeys, size_t rowBegin, size_t rowEnd, EnumColumnHealth &health
    );

    /**
     * @brief Shared encode entry that resolves @p column.keys to interned ids.
     */
    bool EncodeColumnRangeAsEnum(
        LogData &data, const Column &column, size_t rowBegin, size_t rowEnd, EnumColumnHealth &health
    );

    /**
     * @brief Rank cache for a `Type::Level` column, or `nullptr`.
     */
    [[nodiscard]] const std::vector<LogLevel> *LevelRankCache(
        const LogData &data, const LogConfiguration &configuration, size_t columnIndex
    ) const noexcept;

private:
    EnumDictionaryRegistry mDictionaries;
    uint16_t mValueCap = DEFAULT_ENUM_VALUE_CAP;
    uint32_t mValueMaxLen = MAX_ENUM_CANDIDATE_LEN;
    std::unordered_map<KeyId, EnumCandidateTracker> mTrackers;
    std::unordered_map<KeyId, EnumColumnHealth> mColumnHealth;
    bool mIsStreaming = false;
    std::vector<KeyId> mLastBatchDemotedKeys;
    std::unordered_map<KeyId, std::vector<LogLevel>> mLevelRankCache;
    std::vector<KeyId> mPendingLevelBubbleKeys;
};

} // namespace loglib
