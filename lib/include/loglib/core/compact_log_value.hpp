/**
 * @brief Compact 16-byte storage for one field of a log row.
 *
 * A CompactLogValue holds a tag and a payload. Strings are not copied into the row: the payload stores an offset and a
 * length that the owning LineSource turns back into bytes. CompactLineFields is the small sorted array of key and value
 * pairs that each LogLine owns. Keeping fields at 16 bytes is what lets a million rows fit in memory.
 *
 * This is part of the core module.
 */
#pragma once

#include "loglib/core/enum_dictionary.hpp"
#include "loglib/core/key_index.hpp"
#include "loglib/core/log_value.hpp"

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
 * @brief Says which kind of value a CompactLogValue holds.
 *
 * The kinds match the alternatives of LogValue, except that strings are kept as an offset and a length.
 */
enum class CompactTag : uint8_t
{
    Monostate = 0,
    /** @brief The payload is an offset for ResolveMmapBytes and aux is the length. */
    MmapSlice,
    /** @brief The payload is an offset for ResolveOwnedBytes and aux is the length. */
    OwnedString,
    /** @brief The payload is an EnumValueId that the registry from LineSource::EnumDictionaries turns into text. */
    DictRef,
    Int64,
    Uint64,
    Double,
    Bool,
    Timestamp,
};

/**
 * @brief The value of one field in 16 bytes.
 *
 * It holds an 8-byte payload, a 4-byte aux value, a tag and padding. A LogValue is built from it only when needed.
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
     * @brief Builds the LogValue this cell stands for.
     *
     * String and enumeration cells need the source and the line id to find their bytes. A DictRef cell also needs the
     * key of its column; with INVALID_KEY_ID it gives std::monostate. With a null source, a MmapSlice cell gives
     * std::monostate and an OwnedString cell gives an empty string.
     */
    LogValue Materialise(const LineSource *source, size_t lineId, KeyId keyId = INVALID_KEY_ID) const;
};

/** @brief Required size of CompactLogValue; growing it inflates every per-line allocation. */
inline constexpr size_t COMPACT_LOG_VALUE_EXPECTED_BYTES = 16;
static_assert(sizeof(CompactLogValue) == COMPACT_LOG_VALUE_EXPECTED_BYTES, "CompactLogValue must stay 16 bytes");

/**
 * @brief Adds @p delta to every OwnedString payload in @p values.
 * @param values Compact field pairs to rebase.
 * @param valueCount Number of pairs in @p values.
 * @param delta Offset applied to each OwnedString payload.
 */
void RebaseOwnedStringOffsets(std::pair<KeyId, CompactLogValue> *values, size_t valueCount, uint64_t delta) noexcept;

/**
 * @brief Per-line compact field storage.
 *
 * Exact-fit heap array of (KeyId, CompactLogValue) pairs sorted by
 * KeyId. Pointer, size, and capacity occupy 16 bytes, avoiding
 * std::vector's reserve overhead on narrow rows.
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
     * The caller must keep the array sorted by KeyId.
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

private:
    value_type *mData = nullptr;
    uint32_t mSize = 0;
    uint32_t mCapacity = 0;
};

/** @brief Required size of CompactLineFields. */
inline constexpr size_t COMPACT_LINE_FIELDS_EXPECTED_BYTES = 16;
static_assert(sizeof(CompactLineFields) == COMPACT_LINE_FIELDS_EXPECTED_BYTES, "CompactLineFields must stay 16 bytes");

} // namespace loglib
