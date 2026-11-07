/**
 * @brief Detects columns that hold only a few distinct strings and encodes them.
 *
 * While rows arrive, EnumInference watches each undecided column. A column with few distinct short values is promoted
 * to an enumeration, and its values are replaced by small ids in an EnumDictionary. A column that grows too many
 * distinct values is demoted again. It also detects level columns and caches the rank of each level for sorting.
 * LogTable owns one and drives it.
 *
 * This is part of the table module.
 */
#pragma once

#include "loglib/core/enum_dictionary.hpp"
#include "loglib/core/key_index.hpp"
#include "loglib/core/log_level.hpp"
#include "loglib/core/transparent_string_hash.hpp"
#include "loglib/schema/column_schema.hpp"

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

/** @brief For each level, the raw dictionary values that mapped to it. */
using LevelValueMap = std::unordered_map<LogLevel, std::vector<std::string>>;

/**
 * @brief Rows a loaded file needs before an undecided column can become an enumeration. Smaller files are decided when
 * the parse ends.
 */
inline constexpr size_t ENUM_PROMOTION_MIN_ROWS = 4096;

/**
 * @brief Rows a stream needs before an undecided column can become an enumeration. Streams promote early, and the
 * value and length caps protect against bad guesses.
 */
inline constexpr size_t STREAM_PROMOTION_MIN_ROWS = 2;

/**
 * @brief For a loaded file, the highest ratio of distinct values to observed rows that still looks like an
 * enumeration.
 */
inline constexpr double ENUM_CARDINALITY_BAIL_RATIO = 0.05;

/** @brief Share of values that may be too long or of the wrong type before an enumeration column is demoted. */
inline constexpr double ENUM_HEALTH_TOLERANCE_RATIO = 0.01;

/** @brief Number of values to see before the tolerance ratio is applied. */
inline constexpr size_t ENUM_HEALTH_MIN_SAMPLES = 50;

/** @brief For Level detection, how many times larger than the recognized level values the dictionary may be. */
inline constexpr size_t LEVEL_DICT_TOLERANCE_RATIO = 4;

/**
 * @brief Collects the evidence for turning one undecided column into an enumeration.
 *
 * It remembers up to cap distinct values, and is killed when a new value would exceed the cap. Values that are too
 * long are only counted. The numeric and boolean counters let a column with no strings end up as a number or boolean
 * type. A sparse column is not given up on before it has been seen at least once, which is why presenceCount and
 * rowsObserved are kept apart. EnumInference owns the trackers.
 */
struct EnumCandidateTracker
{
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    /** @brief The distinct values seen so far, in insertion order, up to the cap. */
    std::vector<std::string> values;
    /**
     * @brief Fast membership test for values. It hashes transparently so a string_view can be looked up without making
     * a string.
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
     * @brief Records one string value. The caller has already counted it in presenceCount. Sets killed when the cap is
     * exceeded.
     */
    void Observe(std::string_view bytes);
};

/**
 * @brief Running count of how well an enumeration column keeps fitting. Values that are too long and values of the
 * wrong type share one budget.
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
 * @brief Chooses the type of a column that has no strings, from how many numbers and booleans it held.
 *
 * - Only booleans gives Boolean.
 * - Integers or doubles give Integer, Floating or Number.
 * - A mix of booleans and numbers gives Any.
 * - Nothing observed gives Any.
 *
 * A column that is already an enumeration and later sees booleans is demoted to String, not Boolean.
 */
[[nodiscard]] ColumnType RouteNoStringBail(
    size_t intObservations, size_t uintObservations, size_t doubleObservations, size_t boolObservations
) noexcept;

/**
 * @brief True if the column takes part in enumeration encoding and detection: always for Enumeration and Level
 * columns, and for Any only with auto-detect on.
 */
[[nodiscard]] bool IsEnumPassEligible(const Column &column) noexcept;

/**
 * @brief The key under which a column's dictionary, health count and level cache are stored: the first of its keys that
 * the key index knows, or INVALID_KEY_ID if none is known yet.
 *
 * Every store and every read of that state goes through this function, so a column whose first configured key appears
 * after one of its aliases still finds its own state.
 */
[[nodiscard]] KeyId CanonicalKeyOf(const KeyIndex &keys, const Column &column);

/**
 * @brief Decides which columns are enumerations and keeps the dictionaries and level cache that follow from it.
 *
 * LogTable owns one. It owns the dictionaries, the candidate trackers, the health counts, the level rank cache and the
 * level columns waiting to move. It borrows the LogData and the configuration for each pass and does not own any rows.
 * Its hot paths are plain member functions, with no per-row virtual calls.
 */
class EnumInference
{
public:
    EnumInference() = default;

    EnumInference(const EnumInference &) = delete;
    EnumInference &operator=(const EnumInference &) = delete;

    /**
     * @brief Moves the state. Line sources keep pointing at the old dictionaries, so the owner must point them at the
     * new ones afterwards.
     */
    EnumInference(EnumInference &&) noexcept = default;
    EnumInference &operator=(EnumInference &&) noexcept = default;

    ~EnumInference() = default;

    /** @brief Drops the dictionaries, trackers, health counts, level cache and the batch records. */
    void Clear();

    /**
     * @brief Clears the inference state and switches to the stream thresholds: promote after STREAM_PROMOTION_MIN_ROWS
     * and skip the cardinality check.
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

    /**
     * @brief Columns demoted from enumeration since the last ClearLastBatchDemotedKeys, including ones promoted and
     * demoted again in between.
     */
    [[nodiscard]] const std::vector<KeyId> &LastBatchDemotedKeys() const noexcept;
    void ClearLastBatchDemotedKeys() noexcept;

    /**
     * @brief Returns the level values of the Level columns demoted since the last call, by canonical key, and forgets
     * them.
     *
     * DemoteColumnFromEnum records the mapping just before it erases the dictionary, which is the last moment it can be
     * built. Levels without a recognized value are left out, and a column with no recognized value has no entry.
     */
    [[nodiscard]] std::unordered_map<KeyId, LevelValueMap> TakeDemotedLevelValues() noexcept;

    /** @brief Returns the columns that became Level columns and are waiting to move, and forgets them. */
    [[nodiscard]] std::vector<KeyId> TakePendingLevelBubbleKeys() noexcept;

    void EraseTracker(KeyId canonical);
    [[nodiscard]] EnumColumnHealth &HealthFor(KeyId canonical);
    void EraseHealthAndLevelCache(KeyId canonical);

    /**
     * @brief Creates the dictionaries of the configured enumeration and Level columns and fills the level cache.
     *
     * The level cache is cleared first, so a newly loaded level mapping takes effect. Calling it again is harmless.
     */
    void RefreshSnapshot(LogData &data, const LogConfigurationManager &configuration);

    /**
     * @brief Runs the enumeration pass over the rows added since oldLineCount.
     *
     * It encodes the rows of enumeration columns, demotes columns that overflowed, and promotes candidates that look
     * settled. The two back-fill parameters are widened to cover any column whose existing rows were re-encoded.
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
     * @brief Finishes detection at the end of a parse or stream. Returns true if a column became an enumeration.
     * Repeating it changes nothing.
     */
    bool Finalize(LogData &data, LogConfigurationManager &configuration);

    /** @brief Runs detection again over all existing rows of a column. Returns the column type afterwards. */
    ColumnType RescanColumn(
        LogData &data,
        LogConfigurationManager &configuration,
        const std::vector<std::vector<KeyId>> &columnKeyIds,
        size_t columnIndex
    );

    /** @brief Makes the column an enumeration and encodes every existing row's value as a DictRef cell. */
    void PromoteColumnToEnum(LogData &data, LogConfigurationManager &configuration, size_t columnIndex);

    /**
     * @brief Makes the column a String column again, turning every DictRef cell back into an owned string and dropping
     * the dictionary.
     *
     * With recordForBatch the key is added to LastBatchDemotedKeys. The column editor passes false.
     */
    void DemoteColumnFromEnum(
        LogData &data, LogConfigurationManager &configuration, size_t columnIndex, bool recordForBatch = true
    );

    /**
     * @brief Turns an enumeration column into a Level column if its key names a level and its values look like levels.
     *
     * The dictionary must not be larger than LEVEL_DICT_TOLERANCE_RATIO times its recognized values. The column is
     * queued for TakePendingLevelBubbleKeys and is not moved here.
     */
    void MaybePromoteToLevel(LogData &data, LogConfigurationManager &configuration, size_t columnIndex);

    /** @brief Builds or extends the level cache of a Level column. Other columns are ignored. */
    void RefreshLevelRankCache(const LogData &data, const LogConfiguration &configuration, size_t columnIndex);

    /**
     * @brief Encodes the values of a column in the given row range as DictRef cells.
     *
     * Returns false if the dictionary overflowed. Values that are too long or of the wrong type are counted in health.
     */
    bool EncodeColumnRange(
        LogData &data, std::span<const KeyId> aliasKeys, size_t rowBegin, size_t rowEnd, EnumColumnHealth &health
    );

    /** @brief Like EncodeColumnRange, but looks up the key ids from the column's key names. */
    bool EncodeColumnRangeAsEnum(
        LogData &data, const Column &column, size_t rowBegin, size_t rowEnd, EnumColumnHealth &health
    );

    /** @brief The level cache of a Level column, or null for any other column. See LogTable::LevelRankCache. */
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
    std::unordered_map<KeyId, LevelValueMap> mDemotedLevelValues;
};

} // namespace loglib
