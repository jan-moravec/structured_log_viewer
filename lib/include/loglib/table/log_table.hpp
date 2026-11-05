/**
 * @brief Table of parsed rows with their column configuration.
 *
 * LogTable holds the rows of a static file or a live stream, and the LogConfigurationManager that describes its
 * columns. Parsers feed it batches through AppendBatch. It decides column types as data arrives, backfills timestamps,
 * evicts old rows from live streams and exposes the cells to filters, sorting and exporters. The interface wraps it in
 * a model.
 *
 * This is part of the table module.
 */
#pragma once

#include "loglib/core/enum_dictionary.hpp"
#include "loglib/core/key_index.hpp"
#include "loglib/core/line_source.hpp"
#include "loglib/core/log_data.hpp"
#include "loglib/core/log_file.hpp"
#include "loglib/core/log_level.hpp"
#include "loglib/core/row_batch.hpp"
#include "loglib/schema/log_configuration.hpp"
#include "loglib/table/column_type_health.hpp"
#include "loglib/table/enum_inference.hpp"
#include "loglib/time/time_zone_context.hpp"

#include <chrono>
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
 * @brief The rows of a log as a table with typed columns, for static files and live streams alike.
 *
 * A LogTable owns the LogData and the column configuration, and turns a row and column number into a value. It also
 * splices streamed batches in and evicts the oldest rows. Type inference, enumeration dictionaries and the level rank
 * cache live in EnumInference, which the table owns and drives. Column health is computed by ComputeColumnTypeHealth.
 *
 * Two time zones belong to the table, and both are UTC until set. The display zone formats timestamp cells. The default
 * source zone is how the table reads text that carries no offset when it converts a column itself. The table never
 * reads the process-wide default zone, so two tables in one process can use different zones. Moves and Reset keep both.
 *
 * The table does not notify anyone of changes. A view over it must announce column moves, row removals and similar
 * changes before calling the mutating member.
 */
class LogTable
{
public:
    LogTable() = default;

    LogTable(LogData data, LogConfigurationManager configuration);

    LogTable(const LogTable &) = delete;
    LogTable &operator=(const LogTable &) = delete;

    /** @brief Moves the table and points every owned LineSource at the moved enumeration dictionaries. */
    LogTable(LogTable &&) noexcept;
    LogTable &operator=(LogTable &&) noexcept;
    ~LogTable() = default;

    /** @brief Replaces the table's data with data, a freshly merged load. */
    void Update(LogData &&data);

    /** @brief Removes all rows and sources. The column configuration is kept. */
    void Reset();

    /**
     * @brief Prepares the table for a streaming parse that starts with source, which may be null.
     *
     * The configuration must not change until streaming finishes.
     */
    void BeginStreaming(std::unique_ptr<LineSource> source);

    /**
     * @brief Adds another source to a streaming parse in progress, for example the next file of a rotated series.
     * Nothing is reset.
     */
    void AppendStreaming(std::unique_ptr<LineSource> source);

    /** @brief What one AppendBatch call changed beyond the new rows. */
    struct AppendBatchResult
    {
        /** @brief Inclusive [firstColumn, lastColumn] back-filled by the batch, or nullopt. */
        std::optional<std::pair<size_t, size_t>> backfilledColumns;

        /**
         * @brief Canonical KeyIds of columns demoted away from Type::Enumeration by the batch. Includes the silent
         * case where a column was promoted and demoted within the same batch, which a before and after comparison of
         * dictionary sizes cannot see.
         */
        std::vector<KeyId> demotedKeys;

        /**
         * @brief Canonical KeyIds of columns the batch flipped to Type::Level. The bubble to
         * CANONICAL_LEVEL_COLUMN_INDEX is left to the caller, which moves each column itself so a view can announce
         * every move before it happens. Re-resolve each KeyId to its current column index before moving, because
         * earlier moves can shift later targets.
         */
        std::vector<KeyId> levelBubbleKeys;
    };

    /**
     * @brief Adds a streamed batch of rows to the table.
     *
     * New keys in the batch become new columns, and new time columns are filled in for the rows already in the table.
     * The result lists what else changed: columns that must move and columns that stopped being enumerations. The
     * caller must act on it.
     */
    [[nodiscard]] AppendBatchResult AppendBatch(RowBatch batch);

    /**
     * @brief Reports how many rows and columns AppendBatch would add, without changing the table.
     *
     * Use it to tell a view about the growth before calling AppendBatch.
     */
    struct AppendBatchPreview
    {
        size_t newRowCount = 0;
        size_t newColumnCount = 0;
    };
    [[nodiscard]] AppendBatchPreview PreviewAppend(const RowBatch &batch) const;

    /**
     * @brief Moves a column from srcIndex to destIndex.
     *
     * The table does not notify anyone. A view over the table must announce the move before calling this.
     */
    void MoveColumn(size_t srcIndex, size_t destIndex);

    /**
     * @brief Lists the time columns that belong at the front, among the columns from firstColumn on.
     *
     * The result holds the indices of Type::Time columns in ascending order, skipping index 0. Moving each one to index
     * 0 in that order keeps the later indices valid. A streaming consumer passes the column count from before
     * AppendBatch, so new time columns come first as they do after a static load.
     */
    [[nodiscard]] std::vector<size_t> TimeColumnsToFront(size_t firstColumn) const;

    /**
     * @brief Adds a batch and arranges the columns, without any notification.
     *
     * It runs AppendBatch, moves each new time column to the front, then moves the new level columns. A host that must
     * announce every move calls AppendBatch and the move steps itself.
     */
    void AppendBatchArranged(RowBatch batch);

    /**
     * @brief Moves each column named in keys to CANONICAL_LEVEL_COLUMN_INDEX, unless ShouldBubbleLevelColumn says it
     * should stay.
     */
    void BubbleLevelColumns(std::span<const KeyId> keys);

    /**
     * @brief The index of the first column that uses kid, or -1. The search is linear, which is fine for a few dozen
     * columns.
     */
    [[nodiscard]] int FindColumnIndexByKey(KeyId kid) const noexcept;

    /** @brief Reserves room for count line offsets in the first source, if it is a file. Other sources ignore it. */
    void ReserveLineOffsets(size_t count);

    [[nodiscard]] std::string GetHeader(size_t column) const;
    [[nodiscard]] size_t ColumnCount() const;
    [[nodiscard]] LogValue GetValue(size_t row, size_t column) const;
    [[nodiscard]] std::string GetFormattedValue(size_t row, size_t column) const;

    /**
     * @brief Returns a cell as text, formatting it if it is not already a string.
     *
     * A string cell is returned as a view into the file, the source or the dictionary, and stays valid as long as
     * they do. Any other cell is formatted with the column's print format into buffer, and the result is a view of
     * buffer, so keep buffer alive while using it. A missing cell gives an empty view. This does one lookup for both
     * cases, which keeps string filters cheap.
     */
    [[nodiscard]] std::string_view GetValueOrFormatted(size_t row, size_t column, std::string &buffer) const;

    [[nodiscard]] size_t RowCount() const;

    /**
     * @brief The rows and sources behind the table.
     *
     * Change them only through the table's own members (AppendBatch, Update, Reset, EvictPrefixRows). Rows inserted
     * directly skip the timestamp back-fill and the enumeration pass.
     */
    [[nodiscard]] const LogData &Data() const noexcept;
    [[nodiscard]] LogData &Data() noexcept;

    /**
     * @brief Drops the first count rows.
     *
     * The table does not notify anyone. A view over the table must announce the removal before calling this.
     */
    void EvictPrefixRows(size_t count);

    /**
     * @brief The key index shared by the table and its rows.
     *
     * Parser threads use the mutable overload to intern keys before the batch that uses them is appended. It is the
     * same index as Data().Keys(), and it must not be replaced or cleared while rows still use its ids.
     */
    KeyIndex &Keys();
    const KeyIndex &Keys() const;

    /**
     * @brief The dictionaries of all enumeration columns. A filter picker lists the values of one with
     * Find(keyId)->Values().
     */
    const EnumDictionaryRegistry &EnumDictionaries() const noexcept;

    /**
     * @brief Sets how many distinct values an enumeration column may hold, from 1 to MAX_ENUM_VALUES. Existing
     * dictionaries keep their cap. For tests and tuning.
     */
    void SetEnumValueCap(uint16_t cap) noexcept;

    [[nodiscard]] uint16_t EnumValueCap() const noexcept;

    /**
     * @brief Sets the longest value that still looks like an enumeration value. Zero turns the limit off. Longer
     * values count against the column's health. For tests and tuning.
     */
    void SetEnumValueMaxLen(uint32_t maxLen) noexcept;

    [[nodiscard]] uint32_t EnumValueMaxLen() const noexcept;

    /**
     * @brief The enumeration value id of a cell, or nullopt if the cell is not a DictRef. Lets filters compare ids
     * instead of text.
     */
    [[nodiscard]] std::optional<EnumValueId> GetEnumValueId(size_t row, size_t column) const noexcept;

    /**
     * @brief The level of a cell in a Type::Level column.
     *
     * Returns nullopt for other column types, for empty cells, and for values that do not map to a level. Sorting and
     * styling use this, so unmapped values stay unstyled.
     */
    [[nodiscard]] std::optional<LogLevel> GetLevelForRow(size_t row, size_t columnIndex) const noexcept;

    /**
     * @brief Like GetLevelForRow, but a value that exists and does not map to a level gives LogLevel::Unknown.
     *
     * Use it to draw a generic icon for such values. A value whose dictionary id the rank cache has not caught up
     * with yet, which can happen while streaming, also gives Unknown. Nullopt still means there is no value.
     */
    [[nodiscard]] std::optional<LogLevel> GetDisplayLevelForRow(size_t row, size_t columnIndex) const noexcept;

    /**
     * @brief Maps each enumeration value id of a Level column to its level.
     *
     * The entry at an id is LogLevel::Unknown if that value does not name a level. Returns null if the column is not a
     * Level column, the index is out of range, or the column's key has not been seen yet. Caches are kept per key, so
     * two Level columns with the same header do not share one.
     */
    [[nodiscard]] const std::vector<LogLevel> *LevelRankCache(size_t columnIndex) const noexcept;

    /**
     * @brief The result of ResolveEnumColumn.
     *
     * - canonicalKey is INVALID_KEY_ID: the column does not exist, has no keys, or its first key is not interned. Skip
     *   enumeration handling.
     * - canonicalKey is valid and dictionary is null: the column is not an enumeration now, so predicates compare
     *   text.
     * - Both are set: the column is an enumeration, and the dictionary can be used directly.
     */
    struct EnumColumnLookup
    {
        KeyId canonicalKey = INVALID_KEY_ID;
        const EnumDictionary *dictionary = nullptr;
    };

    /** @brief Finds the canonical key and the dictionary of a column. Any miss gives a default-constructed lookup. */
    [[nodiscard]] EnumColumnLookup ResolveEnumColumn(size_t columnIndex) const noexcept;

    /**
     * @brief What FinalizeAutoDetection changed.
     */
    struct FinalizeResult
    {
        /** @brief True if at least one column became an enumeration. */
        bool promotedToEnumeration = false;

        /** @brief Columns that became Level columns. Move them as described for AppendBatchResult. */
        std::vector<KeyId> levelBubbleKeys;
    };

    /**
     * @brief Finishes type detection when a parse or stream ends.
     *
     * Columns that are still undecided are promoted to enumerations where the evidence allows. Columns of type Any
     * with auto-detect on get a final type, or stay as they are when there is too little data. Calling it again does
     * nothing more. The caller must act on the result.
     */
    [[nodiscard]] FinalizeResult FinalizeAutoDetection();

    /**
     * @brief Brings the loaded rows in line with a column type the user just changed.
     *
     * The caller has already stored the new type and auto-detect flag in the configuration. This updates the rows now,
     * because a fully loaded static file would never get another batch to do it.
     *
     * - Time: adds default formats if there are none, then converts every row.
     * - Enumeration or Level: creates the dictionary, encodes every cell, and refreshes the level cache.
     * - Any other type: removes the dictionary and cache and turns encoded cells back into strings. If previousType was
     *   Time, the print and parse formats are cleared too.
     *
     * previousType also lets a change that does nothing be skipped, for example turning auto-detect on or off for an
     * enumeration column, which keeps its accumulated health. An out-of-range column is ignored.
     */
    void OnUserChangedColumnType(size_t columnIndex, ColumnType previousType);

    /**
     * @brief Refreshes the per-column caches after the configuration was replaced by loading one.
     *
     * Reset refreshes against the old configuration, so call this once the new columns are in place. It is safe on an
     * empty table.
     */
    void OnConfigurationReloaded();

    /**
     * @brief Runs type detection again over all rows of a column, as if they had just streamed in.
     *
     * Use it when the user sets a column of a loaded file back to Any with auto-detect, because no further batch will
     * arrive to trigger detection. It does nothing unless the column is Any with auto-detect, and does nothing on an
     * empty table or an out-of-range column. Returns the column type afterwards.
     */
    ColumnType RescanColumnForAutoDetection(size_t columnIndex);

    using ColumnTypeHealth = loglib::ColumnTypeHealth;

    /** @brief Checks whether the data of a column fits its configured type. See ComputeColumnTypeHealth. */
    [[nodiscard]] ColumnTypeHealth ComputeColumnTypeHealth(size_t columnIndex) const;

    /** @brief Sets the zone in which timestamp cells are formatted. UTC by default. */
    void SetDisplayTimeZone(TimeZoneContext timeZone);

    /** @brief The zone in which timestamp cells are formatted. */
    [[nodiscard]] const TimeZoneContext &DisplayTimeZone() const noexcept;

    /**
     * @brief Sets the zone in which the table reads timestamp text that carries no offset, when it converts a column
     * after the rows exist. UTC by default. Pass the same zone to ParserOptions::defaultSourceTimeZone, so both agree.
     */
    void SetDefaultSourceTimeZone(TimeZoneContext timeZone);

    /** @brief The zone in which the table reads timestamp text that carries no offset. */
    [[nodiscard]] const TimeZoneContext &DefaultSourceTimeZone() const noexcept;

    /**
     * @brief Fixes the "now" the table uses to convert timestamp formats without a year. Without a call, the table
     * reads the clock when it is created and again when BeginStreaming starts a parse, so one parse sees one year.
     */
    void SetReferenceTime(std::chrono::system_clock::time_point referenceTime) noexcept;

    /** @brief The "now" the table uses to convert timestamp formats without a year. */
    [[nodiscard]] std::chrono::system_clock::time_point ReferenceTime() const noexcept;

    const LogConfigurationManager &Configuration() const;

    /** @brief The configuration for loading and saving. Do not change it while a streaming parse runs. */
    LogConfigurationManager &Configuration();

private:
    /**
     * @brief Formats a cell with the column's print format and the display zone. It does not throw: a print format that
     * the value does not accept gives the value's plain text.
     */
    [[nodiscard]] std::string FormatLogValue(const std::string &format, const LogValue &value) const;

    void RefreshColumnKeyIds();
    void RefreshColumnKeyIdsForKeys(const std::vector<std::string> &newKeys);

    /** @brief Points every owned LineSource at the enumeration dictionaries. */
    void RewireSourceRegistries();

    LogData mData;
    LogConfigurationManager mConfiguration;

    TimeZoneContext mDisplayTimeZone;
    TimeZoneContext mDefaultSourceTimeZone;
    std::chrono::system_clock::time_point mReferenceTime = std::chrono::system_clock::now();
    bool mReferenceTimeFixed = false;

    /** @brief The key ids of each column, indexed by column. */
    std::vector<std::vector<KeyId>> mColumnKeyIds;

    /**
     * @brief The time keys that the table converts itself, because the parser did not. The table has converted the rows
     * it already held once, and converts each later batch as it arrives.
     */
    std::unordered_set<KeyId> mTableConvertedTimeKeys;

    /**
     * @brief Type inference, enumeration encoding and the level cache. It owns the dictionaries, which LineSources
     * borrow.
     */
    EnumInference mEnum;
};

} // namespace loglib
