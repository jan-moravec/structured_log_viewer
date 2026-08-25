#pragma once

#include "loglib/enum_dictionary.hpp"
#include "loglib/key_index.hpp"
#include "loglib/log_value.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace loglib
{
class LineSource;
}

namespace loglib
{

/**
 * @brief Discriminator for `CompactLogValue`.
 *
 * Mirrors `LogValue`'s alternatives but stores strings as
 * `(offset, length)` pairs.
 */
enum class CompactTag : uint8_t
{
    Monostate = 0,
    /** @brief `payload` is an offset into `ResolveMmapBytes`; `aux` is the length. */
    MmapSlice,
    /** @brief `payload` is an offset into `ResolveOwnedBytes`; `aux` is the length. */
    OwnedString,
    /** @brief `payload` is an `EnumValueId`; resolved via `LineSource::EnumDictionaries`. */
    DictRef,
    Int64,
    Uint64,
    Double,
    Bool,
    Timestamp,
};

/**
 * @brief 16-byte tagged union for per-field storage.
 *
 * Layout is 8 bytes of payload, 4 bytes of aux, 1 byte of tag, and
 * padding. `LogValue` is materialised on demand.
 */
struct CompactLogValue
{
    uint64_t payload = 0;
    uint32_t aux = 0;
    CompactTag tag = CompactTag::Monostate;
    uint8_t pad0 = 0;
    uint8_t pad1 = 0;
    uint8_t pad2 = 0;

    static CompactLogValue MakeMonostate() noexcept;
    static CompactLogValue MakeMmapSlice(uint64_t offset, uint32_t length) noexcept;
    static CompactLogValue MakeOwnedString(uint64_t offset, uint32_t length) noexcept;
    static CompactLogValue MakeDictRef(EnumValueId id) noexcept;
    static CompactLogValue MakeInt64(int64_t value) noexcept;
    static CompactLogValue MakeUint64(uint64_t value) noexcept;
    static CompactLogValue MakeDouble(double value) noexcept;
    static CompactLogValue MakeBool(bool value) noexcept;
    static CompactLogValue MakeTimestamp(TimeStamp value) noexcept;

    /**
     * @brief Materialises this slot into a `LogValue`.
     *
     * For `DictRef`, pass `INVALID_KEY_ID` to materialise as
     * monostate. A null source is safe.
     *
     * @param source Line source used to resolve string and dictionary
     *     payloads, or `nullptr`.
     * @param lineId Line id within @p source.
     * @param keyId Key used to resolve `DictRef` payloads.
     * @return The materialised value.
     */
    LogValue Materialise(const LineSource *source, size_t lineId, KeyId keyId = INVALID_KEY_ID) const;
};

/** @brief Required size of `CompactLogValue`; growing it inflates every per-line allocation. */
inline constexpr size_t COMPACT_LOG_VALUE_EXPECTED_BYTES = 16;
static_assert(sizeof(CompactLogValue) == COMPACT_LOG_VALUE_EXPECTED_BYTES, "CompactLogValue must stay 16 bytes");

/**
 * @brief Converts a `LogValue` to a `CompactLogValue`.
 *
 * Strings inside `[fileBegin, fileBegin + fileSize)` become
 * `MmapSlice`; others are copied into @p ownedStringArena.
 *
 * @param value Value to compact.
 * @param ownedStringArena Arena that receives owned string bytes.
 * @param fileBegin Start of the mmap window, or `nullptr`.
 * @param fileSize Size of the mmap window in bytes.
 * @return The compact representation.
 */
CompactLogValue ToCompactLogValue(
    const LogValue &value, std::string &ownedStringArena, const char *fileBegin = nullptr, size_t fileSize = 0
);

/**
 * @brief Adds @p delta to every `OwnedString` payload in @p values.
 * @param values Compact field pairs to rebase.
 * @param valueCount Number of pairs in @p values.
 * @param delta Offset applied to each `OwnedString` payload.
 */
void RebaseOwnedStringOffsets(std::pair<KeyId, CompactLogValue> *values, size_t valueCount, uint64_t delta) noexcept;

/**
 * @brief Per-line compact field storage.
 *
 * Exact-fit heap array of `(KeyId, CompactLogValue)` pairs sorted by
 * `KeyId`. Pointer, size, and capacity occupy 16 bytes, avoiding
 * `std::vector`'s reserve overhead on narrow rows.
 */
class CompactLineFields
{
public:
    using value_type = std::pair<KeyId, CompactLogValue>;

    CompactLineFields() = default;

    /**
     * @brief Allocates @p initialCapacity slots without constructing them.
     * @param initialCapacity Number of slots to reserve.
     */
    explicit CompactLineFields(uint32_t initialCapacity);

    CompactLineFields(const CompactLineFields &) = delete;
    CompactLineFields &operator=(const CompactLineFields &) = delete;

    CompactLineFields(CompactLineFields &&other) noexcept;
    CompactLineFields &operator=(CompactLineFields &&other) noexcept;
    ~CompactLineFields();

    /** @brief Returns the number of occupied slots. */
    [[nodiscard]] uint32_t Size() const noexcept
    {
        return mSize;
    }

    /** @brief Returns the allocated slot count. */
    [[nodiscard]] uint32_t Capacity() const noexcept
    {
        return mCapacity;
    }

    /** @brief Returns whether the array has no occupied slots. */
    [[nodiscard]] bool Empty() const noexcept
    {
        return mSize == 0;
    }

    /** @brief Returns a pointer to the first slot. */
    [[nodiscard]] value_type *Data() noexcept
    {
        return mData;
    }

    /** @brief Returns a pointer to the first slot. */
    [[nodiscard]] const value_type *Data() const noexcept
    {
        return mData;
    }

    [[nodiscard]] value_type *begin() noexcept
    {
        return mData;
    }

    [[nodiscard]] value_type *end() noexcept
    {
        return mData + mSize;
    }

    [[nodiscard]] const value_type *begin() const noexcept
    {
        return mData;
    }

    [[nodiscard]] const value_type *end() const noexcept
    {
        return mData + mSize;
    }

    /**
     * @brief Grows capacity to at least @p capacity.
     * @param capacity Requested slot count. No-op if already large enough.
     */
    void Reserve(uint32_t capacity);

    /**
     * @brief Replaces contents with @p values.
     *
     * Reuses the buffer when it is large enough; otherwise reallocates
     * exact-fit.
     *
     * @param values Sorted field pairs.
     * @param count Number of pairs in @p values.
     */
    void AssignSorted(const value_type *values, uint32_t count);
    void AssignSorted(std::vector<value_type> &&values);

    /**
     * @brief Appends a pair at the end.
     *
     * The caller must keep the array sorted by `KeyId`.
     *
     * @param key Field key.
     * @param value Compact field value.
     */
    void EmplaceBack(KeyId key, CompactLogValue value);

    /**
     * @brief Inserts a pair at @p position.
     * @param position Insertion index; later slots shift by one.
     * @param key Field key.
     * @param value Compact field value.
     */
    void Insert(uint32_t position, KeyId key, CompactLogValue value);

    /**
     * @brief Replaces the value at @p position.
     * @param position Slot index.
     * @param value New compact value.
     */
    void Set(uint32_t position, CompactLogValue value) noexcept;

    /** @brief Returns heap bytes owned by this array. */
    [[nodiscard]] size_t OwnedMemoryBytes() const noexcept;

    /** @brief Shrinks capacity to match `Size()`. */
    void ShrinkToFit();

private:
    value_type *mData = nullptr;
    uint32_t mSize = 0;
    uint32_t mCapacity = 0;
};

/** @brief Required size of `CompactLineFields`. */
inline constexpr size_t COMPACT_LINE_FIELDS_EXPECTED_BYTES = 16;
static_assert(sizeof(CompactLineFields) == COMPACT_LINE_FIELDS_EXPECTED_BYTES, "CompactLineFields must stay 16 bytes");

} // namespace loglib
