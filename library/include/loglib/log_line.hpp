#pragma once

#include "loglib/compact_log_value.hpp"
#include "loglib/enum_dictionary.hpp"
#include "loglib/key_index.hpp"
#include "loglib/log_value.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace loglib
{

class LineSource;

/**
 * @brief One log record: a `(KeyId, CompactLogValue)` vector sorted by KeyId.
 * Strings are stored as `(offset, length)` resolved via the owning
 * `LineSource`. `(LineSource*, lineId)` is the row's session identity.
 */
class LogLine
{
public:
    /**
     * @brief Cold path: convert variant values to compact storage. Views inside
     * `StableBytes()` become `MmapSlice`; others are copied to the arena.
     */
    LogLine(
        std::vector<std::pair<KeyId, LogValue>> sortedValues, const KeyIndex &keys, LineSource &source, size_t lineId
    );

    /**
     * @brief Hot path: pre-built compact values, ascending on `pair::first`.
     * `OwnedString` payloads must be arena-relative already.
     */
    LogLine(
        std::vector<std::pair<KeyId, loglib::CompactLogValue>> sortedValues,
        const KeyIndex &keys,
        LineSource &source,
        size_t lineId
    );

    /** @brief Cold-path convenience ctor. */
    LogLine(const LogMap &values, KeyIndex &keys, LineSource &source, size_t lineId);

    LogLine(const LogLine &) = delete;
    LogLine &operator=(const LogLine &) = delete;

    LogLine(LogLine &&) = default;
    LogLine &operator=(LogLine &&) = default;

    /** @brief Returns `std::monostate` if @p id is not present. */
    LogValue GetValue(KeyId id) const;
    LogValue GetValue(const std::string &key) const;

    /** @brief Debug builds assert @p value is not a `string_view`. */
    void SetValue(KeyId id, const LogValue &value);

    /**
     * @brief Caller promises any view in @p value outlives the `LogLine`. Views
     * inside `StableBytes()` are stored zero-copy; others are copied.
     */
    void SetValue(KeyId id, const LogValue &value, LogValueTrustView trust);

    /** @brief Throws if @p key is unknown. */
    void SetValue(const std::string &key, const LogValue &value);

    /**
     * @brief Replace/insert the slot for @p id with `DictRef(vid)`. Cold-path
     * helper; the encode hot path uses `FindCompactMutable`.
     */
    void SetOrReplaceEnumDictRef(KeyId id, EnumValueId vid);

    std::vector<std::string> GetKeys() const;

    /** @brief (KeyId, LogValue) pairs in ascending KeyId order. Cold path. */
    std::vector<std::pair<KeyId, LogValue>> IndexedValues() const;

    /** @brief Span over the compact storage; for hot-path walkers. */
    std::span<const std::pair<KeyId, loglib::CompactLogValue>> CompactValues() const noexcept;

    LogMap Values() const;

    /** @brief Used by `LogData::Merge` and `LogData` move-ops. */
    void RebindKeys(const KeyIndex &keys);

    const KeyIndex &Keys() const;

    /** @brief The owning `LineSource`. Never null post-construction. */
    [[nodiscard]] LineSource *Source() const noexcept;

    /** @brief Line id within the source's id space. */
    [[nodiscard]] size_t LineId() const noexcept;

    /** @brief Stage C shifts per-batch relative ids to absolute. */
    void ShiftLineId(size_t delta) noexcept;

    /** @brief Cold-path mutator (e.g. overwrite a placeholder id). */
    void SetLineId(size_t lineId) noexcept;

    /** @brief Heap bytes owned directly by this line (benchmark-only). */
    size_t OwnedMemoryBytes() const;

    size_t ValueCount() const noexcept;

    /**
     * @brief Add @p delta to every `OwnedString` payload after rebasing the
     * source arena.
     */
    void RebaseOwnedStringOffsets(uint64_t delta) noexcept;

    bool IsMmapSlice(KeyId id) const noexcept;
    bool IsOwnedString(KeyId id) const noexcept;
    bool IsDictRef(KeyId id) const noexcept;

    /** @brief `EnumValueId` payload for a `DictRef` slot, else nullopt. */
    [[nodiscard]] std::optional<EnumValueId> GetEnumValueId(KeyId id) const noexcept;

    /** @brief Linear scan over the sorted compact storage; nullptr if absent. */
    [[nodiscard]] const loglib::CompactLogValue *FindCompact(KeyId id) const noexcept;

    /** @brief Mutable counterpart; callers may overwrite `*slot` in place. */
    [[nodiscard]] loglib::CompactLogValue *FindCompactMutable(KeyId id) noexcept;

    /**
     * @brief Bytes for a `MmapSlice` / `OwnedString` slot; nullopt otherwise.
     * Safe with a null source.
     */
    [[nodiscard]] std::optional<std::string_view> PeekStringView(KeyId id) const noexcept;

    /** @brief Slot-pointer overload (skips the linear scan). */
    [[nodiscard]] std::optional<std::string_view> PeekStringView(const loglib::CompactLogValue &slot) const noexcept;

private:
    /** @brief Replace/insert the slot for @p id with @p compact; may allocate. */
    void SetCompact(KeyId id, loglib::CompactLogValue compact);

    loglib::CompactLineFields mValues;
    const KeyIndex *mKeys = nullptr;
    LineSource *mSource = nullptr;
    size_t mLineId = 0;
};

} // namespace loglib
