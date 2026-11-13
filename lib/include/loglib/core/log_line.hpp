/**
 * @brief One parsed log row.
 *
 * A LogLine is a small array of fields, sorted by key, plus the source and line number it came from. Strings stay
 * as offsets into the source, so a row is cheap to create and keep. This header also defines the span type that marks
 * the physical lines of a multi-line record.
 *
 * This is part of the core module.
 */
#pragma once

#include "loglib/core/compact_log_value.hpp"
#include "loglib/core/enum_dictionary.hpp"
#include "loglib/core/key_index.hpp"
#include "loglib/core/log_value.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace loglib
{

class LineSource;

/** @brief The physical lines of one multi-line record, first and last included. Both are zero-based line numbers. */
struct MultiLineRecordSpan
{
    size_t headerLineId = 0;
    size_t lastLineId = 0;
};

/**
 * @brief One log record, stored as key and value pairs sorted by key.
 *
 * Strings are kept as offset and length pairs that the row's LineSource resolves. The source pointer and the line id
 * together identify the row.
 */
class LogLine
{
public:
    /**
     * @brief Builds a row from ordinary values, sorted by key. This is the slow path.
     *
     * A string view that points into the source's stable bytes is stored as a slice; other strings are copied.
     */
    LogLine(std::vector<std::pair<KeyId, LogValue>> sortedValues, LineSource &source, size_t lineId);

    /**
     * @brief Builds a row from fields that are already compact, carry their keys and are sorted by key. OwnedString
     * offsets must already be relative to the source's arena. This is the fast path.
     */
    LogLine(std::vector<loglib::CompactLogValue> sortedValues, LineSource &source, size_t lineId);

    /**
     * @brief Builds a row from compact values that are sorted by key, and keeps its fields in memory from arena. The
     * caller must hand the arena to the source, which has to keep it as long as the row lives. The row moves its fields
     * to the heap if one is added later.
     */
    LogLine(
        std::span<const loglib::CompactLogValue> sortedValues, FieldArena &arena, LineSource &source, size_t lineId
    );

    /**
     * @brief Builds a row from a map, adding unknown keys to keys. Slow, meant for tests.
     *
     * The row does not keep keys. Pass the same index to the name-keyed accessors later.
     */
    LogLine(const LogMap &values, KeyIndex &keys, LineSource &source, size_t lineId);

    LogLine(const LogLine &) = delete;
    LogLine &operator=(const LogLine &) = delete;

    LogLine(LogLine &&) = default;
    LogLine &operator=(LogLine &&) = default;
    ~LogLine() = default;

    /** @brief The value of a field, or std::monostate if the row has none. */
    LogValue GetValue(KeyId id) const;

    /**
     * @brief The value of a field by key name, or std::monostate if the key is unknown or the row has no such field.
     *
     * Slow, because it looks the name up in keys. It is meant for tests and debugging. The index must be the one that
     * the key ids of the row come from.
     */
    LogValue GetValue(const KeyIndex &keys, std::string_view key) const;

    /**
     * @brief Sets a field. The value should not be a string_view, which could dangle: debug builds assert on it. Use
     * the overload with the trust tag for views.
     */
    void SetValue(KeyId id, const LogValue &value);

    /**
     * @brief Sets a field that may be a string_view, and promises that the view outlives the row.
     *
     * A view into the source's stable bytes is stored without copying; any other string is copied.
     */
    void SetValue(KeyId id, const LogValue &value, LogValueTrustView trust);

    /**
     * @brief Sets a field by key name. Throws if the key is not in keys.
     *
     * Slow, meant for tests and debugging.
     */
    void SetValue(const KeyIndex &keys, std::string_view key, const LogValue &value);

    /**
     * @brief The names of the fields of the row, sorted by key id.
     *
     * Slow, meant for tests and debugging. Debug builds assert that every key id is below the size of keys.
     */
    std::vector<std::string> GetKeys(const KeyIndex &keys) const;

    /** @brief All fields as key and value pairs, sorted by key. Slow. */
    std::vector<std::pair<KeyId, LogValue>> IndexedValues() const;

    /** @brief The compact storage itself, for code that walks every field on a hot path. */
    std::span<const loglib::CompactLogValue> CompactValues() const noexcept;

    /**
     * @brief All fields by key name.
     *
     * Slow, meant for tests and debugging. Debug builds assert that every key id is below the size of keys.
     */
    LogMap Values(const KeyIndex &keys) const;

    /** @brief The source that holds this row's bytes. Never null. */
    [[nodiscard]] LineSource *Source() const noexcept;

    /** @brief The row's line id in its source. */
    [[nodiscard]] size_t LineId() const noexcept;

    /** @brief Frees the fields of the row, which then has none. For rows that were evicted but not yet destroyed. */
    void ReleaseFields() noexcept;

    /** @brief Adds delta to the line id. Used when batch-relative ids become absolute. */
    void ShiftLineId(size_t delta) noexcept;

    /** @brief Heap bytes owned by this row alone. For the memory benchmark. */
    size_t OwnedMemoryBytes() const;

    size_t ValueCount() const noexcept;

    /** @brief Adds delta to every OwnedString offset, after the row's strings moved within the source's arena. */
    void RebaseOwnedStringOffsets(uint64_t delta) noexcept;

    bool IsDictRef(KeyId id) const noexcept;

    /** @brief The enumeration value id if the field is a DictRef cell, otherwise nullopt. */
    [[nodiscard]] std::optional<EnumValueId> GetEnumValueId(KeyId id) const noexcept;

    /** @brief The compact cell for a key, or null if the row has none. Searches the row's fields one by one. */
    [[nodiscard]] const loglib::CompactLogValue *FindCompact(KeyId id) const noexcept;

    /** @brief Like FindCompact, but the caller may overwrite the cell in place. */
    [[nodiscard]] loglib::CompactLogValue *FindCompactMutable(KeyId id) noexcept;

    /** @brief The bytes of a string cell, or nullopt if the field is not a string or the row has no source. */
    [[nodiscard]] std::optional<std::string_view> PeekStringView(KeyId id) const noexcept;

    /** @brief Like the key overload, for a cell already found, which saves the search. */
    [[nodiscard]] std::optional<std::string_view> PeekStringView(const loglib::CompactLogValue &slot) const noexcept;

private:
    /** @brief Replaces the cell for id, or inserts it. May allocate. */
    void SetCompact(KeyId id, loglib::CompactLogValue compact);

    loglib::CompactLineFields mValues;
    LineSource *mSource = nullptr;
    size_t mLineId = 0;
};

// A log holds millions of rows, and every filter, sort and export scans them in order. A row is 32 bytes: its fields
// (16 bytes), the source pointer (8 bytes) and the line id (8 bytes). Two rows then fit in one cache line of 64 bytes.
// A new member would add its size to every row of every log.
inline constexpr size_t LOG_LINE_EXPECTED_BYTES = 32;
static_assert(sizeof(LogLine) == LOG_LINE_EXPECTED_BYTES, "LogLine must stay 32 bytes");

} // namespace loglib
