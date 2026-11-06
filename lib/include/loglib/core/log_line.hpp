/**
 * @brief One parsed log row.
 *
 * A LogLine is a small sorted array of key and value pairs plus the source and line number it came from. Strings stay
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
    LogLine(
        std::vector<std::pair<KeyId, LogValue>> sortedValues, const KeyIndex &keys, LineSource &source, size_t lineId
    );

    /**
     * @brief Builds a row from values that are already compact and sorted by key. OwnedString offsets must already be
     * relative to the source's arena. This is the fast path.
     */
    LogLine(
        std::vector<std::pair<KeyId, loglib::CompactLogValue>> sortedValues,
        const KeyIndex &keys,
        LineSource &source,
        size_t lineId
    );

    LogLine(std::vector<std::pair<KeyId, LogValue>> sortedValues, LineSource &source, size_t lineId);
    LogLine(std::vector<std::pair<KeyId, loglib::CompactLogValue>> sortedValues, LineSource &source, size_t lineId);

    /** @brief Builds a row from a map, adding unknown keys to keys. Slow, meant for tests. */
    LogLine(const LogMap &values, KeyIndex &keys, LineSource &source, size_t lineId);

    LogLine(const LogLine &) = delete;
    LogLine &operator=(const LogLine &) = delete;

    LogLine(LogLine &&) = default;
    LogLine &operator=(LogLine &&) = default;
    ~LogLine() = default;

    /** @brief The value of a field, or std::monostate if the row has none. */
    LogValue GetValue(KeyId id) const;
    LogValue GetValue(const std::string &key) const;
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

    /** @brief Sets a field by key name. Throws if the key is unknown. */
    void SetValue(const std::string &key, const LogValue &value);
    void SetValue(const KeyIndex &keys, std::string_view key, const LogValue &value);

    std::vector<std::string> GetKeys() const;
    std::vector<std::string> GetKeys(const KeyIndex &keys) const;

    /** @brief All fields as key and value pairs, sorted by key. Slow. */
    std::vector<std::pair<KeyId, LogValue>> IndexedValues() const;

    /** @brief The compact storage itself, for code that walks every field on a hot path. */
    std::span<const std::pair<KeyId, loglib::CompactLogValue>> CompactValues() const noexcept;

    LogMap Values() const;
    LogMap Values(const KeyIndex &keys) const;

    /**
     * @brief Points the row at a different KeyIndex, after the index moved or the rows were merged into another one.
     */
    void RebindKeys(const KeyIndex &keys);

    const KeyIndex &Keys() const;

    /** @brief The source that holds this row's bytes. Never null. */
    [[nodiscard]] LineSource *Source() const noexcept;

    /** @brief The row's line id in its source. */
    [[nodiscard]] size_t LineId() const noexcept;

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
    const KeyIndex *mKeys = nullptr;
    LineSource *mSource = nullptr;
    size_t mLineId = 0;
};

} // namespace loglib
