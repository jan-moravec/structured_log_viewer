/**
 * @brief Compact 16-byte storage for one field of a log row.
 *
 * A CompactLogValue holds a key id, a tag and a payload. Strings are not copied into the row: the payload stores an
 * offset and a length that the owning LineSource turns back into bytes. CompactLineFields is the small array of
 * fields, sorted by key, that each LogLine owns. Keeping a field at 16 bytes is what lets a million rows fit in
 * memory.
 *
 * This is part of the core module.
 */
#pragma once

#include "loglib/core/enum_dictionary.hpp"
#include "loglib/core/key_index.hpp"
#include "loglib/core/log_value.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
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
 * @brief One field of a row in 16 bytes: the value and the key it belongs to.
 *
 * It holds an 8-byte payload, a 4-byte aux value, a tag and a key id of 24 bits. The Make functions leave the key at
 * zero; the code that puts the field in a row sets it. A LogValue is built from it only when needed.
 */
struct CompactLogValue
{
    uint64_t payload = 0;
    uint32_t aux = 0;
    CompactTag tag = CompactTag::Monostate;
    /** @brief The key id, least significant byte first. Use Key and SetKey. */
    uint8_t keyLow = 0;
    uint8_t keyMid = 0;
    uint8_t keyHigh = 0;

    /**
     * @brief The key id of the field. On a little-endian machine the tag and the three key bytes are one 32-bit word,
     * which one load reads.
     */
    [[nodiscard]] KeyId Key() const noexcept
    {
        if constexpr (std::endian::native == std::endian::little)
        {
            uint32_t word = 0;
            std::memcpy(&word, &tag, sizeof(word));
            return word >> 8U;
        }
        else
        {
            return static_cast<KeyId>(keyLow) | (static_cast<KeyId>(keyMid) << 8U) |
                   (static_cast<KeyId>(keyHigh) << 16U);
        }
    }

    /** @brief Sets the key id. It must not be above MAX_ROW_KEY_ID. */
    void SetKey(KeyId id) noexcept
    {
        keyLow = static_cast<uint8_t>(id & 0xFFU);
        keyMid = static_cast<uint8_t>((id >> 8U) & 0xFFU);
        keyHigh = static_cast<uint8_t>((id >> 16U) & 0xFFU);
    }

    /** @brief A copy of this field with the key id set. */
    [[nodiscard]] CompactLogValue WithKey(KeyId id) const noexcept
    {
        CompactLogValue copy = *this;
        copy.SetKey(id);
        return copy;
    }

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
static_assert(
    offsetof(CompactLogValue, keyHigh) == offsetof(CompactLogValue, tag) + 3U,
    "The tag and the key bytes must be one word"
);

/** @brief Adds delta to the offset of every OwnedString value in the first valueCount fields. */
void RebaseOwnedStringOffsets(CompactLogValue *values, size_t valueCount, uint64_t delta) noexcept;

/**
 * @brief The fields of one line as an array sorted by key, plus a number the owner keeps beside them.
 *
 * The array is exactly as large as needed. A line can also borrow its array from a FieldArena. A borrowed array is
 * never freed by the line, and the first field added to it moves the array to the heap. The size, the flag that says
 * whether the array is owned, and a 39-bit number the owner may keep, such as a line id, share one 8-byte word. The
 * pointer and that word take 16 bytes, so a LogLine that adds only its source pointer is 24 bytes.
 */
class CompactLineFields
{
public:
    using value_type = CompactLogValue;

    /** @brief The largest number of fields in one line. */
    static constexpr uint32_t MAX_FIELDS = 0x7FFFFFU;

    /** @brief The largest value of the number the owner keeps. */
    static constexpr uint64_t MAX_SPARE = (uint64_t{1} << 39U) - 1U;

    CompactLineFields() = default;

    CompactLineFields(const CompactLineFields &) = delete;
    CompactLineFields &operator=(const CompactLineFields &) = delete;

    CompactLineFields(CompactLineFields &&other) noexcept;
    CompactLineFields &operator=(CompactLineFields &&other) noexcept;
    ~CompactLineFields();

    /** @brief Returns the number of fields. */
    [[nodiscard]] uint32_t Size() const noexcept
    {
        return static_cast<uint32_t>(mMeta & SIZE_MASK);
    }

    /** @brief Returns whether the array has no fields. */
    [[nodiscard]] bool Empty() const noexcept
    {
        return Size() == 0;
    }

    /** @brief The number the owner keeps beside the fields. */
    [[nodiscard]] uint64_t Spare() const noexcept
    {
        return mMeta >> SPARE_SHIFT;
    }

    /** @brief Sets the number the owner keeps. It must not be above MAX_SPARE. */
    void SetSpare(uint64_t value) noexcept
    {
        mMeta = (mMeta & ~(~uint64_t{0} << SPARE_SHIFT)) | (value << SPARE_SHIFT);
    }

    /** @brief Returns a pointer to the first field. */
    [[nodiscard]] value_type *Data() noexcept
    {
        return mData;
    }

    /** @brief Returns a pointer to the first field. */
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
        return mData + Size();
    }

    [[nodiscard]] const value_type *begin() const noexcept
    {
        return mData;
    }

    [[nodiscard]] const value_type *end() const noexcept
    {
        return mData + Size();
    }

    /**
     * @brief Replaces the content with count fields that are sorted by key, in a heap array that fits exactly. The
     * spare number stays.
     */
    void AssignSorted(const value_type *values, uint32_t count);

    /** @brief Allocates room for count fields on the heap, sets the size, and returns the fields for the caller to
     * fill. */
    [[nodiscard]] value_type *AllocateOwned(uint32_t count);

    /** @brief Uses count constructed fields at data without owning them. The memory must outlive the fields. */
    void Borrow(value_type *data, uint32_t count) noexcept;

    /** @brief Drops the fields and frees them if they are owned. The spare number stays. */
    void Clear() noexcept;

    /** @brief Inserts a field at position, and the fields after it move up by one. The array moves to the heap. */
    void Insert(uint32_t position, CompactLogValue field);

    /** @brief Replaces the field at position. The new field carries the key. */
    void Set(uint32_t position, CompactLogValue field) noexcept;

    /** @brief Returns heap bytes owned by this array. A borrowed array owns none. */
    [[nodiscard]] size_t OwnedMemoryBytes() const noexcept;

private:
    static constexpr uint64_t SIZE_MASK = 0x7FFFFFU;
    static constexpr uint64_t OWNED_BIT = uint64_t{1} << 23U;
    static constexpr uint32_t SPARE_SHIFT = 24U;

    value_type *mData = nullptr;
    /** @brief The size in bits 0 to 22, the owned flag in bit 23, and the spare number above. */
    uint64_t mMeta = 0;
};

/**
 * @brief Memory for the fields of many rows, handed out in one piece per chunk.
 *
 * A parser worker takes the fields of each row of a batch from one arena, which replaces a heap allocation per row by
 * a pointer bump. The arena goes to the FileLineSource that the rows belong to, which keeps it until the source is
 * destroyed. A chunk never moves, so a pointer from Allocate stays valid for the life of the arena.
 */
class FieldArena
{
public:
    using value_type = CompactLineFields::value_type;

    FieldArena() = default;
    FieldArena(const FieldArena &) = delete;
    FieldArena &operator=(const FieldArena &) = delete;
    FieldArena(FieldArena &&other) noexcept = default;
    FieldArena &operator=(FieldArena &&other) noexcept = default;
    ~FieldArena();

    /** @brief Returns room for count fields that the caller constructs, or null for zero. */
    [[nodiscard]] value_type *Allocate(uint32_t count);

    /** @brief Returns whether nothing was allocated yet. */
    [[nodiscard]] bool Empty() const noexcept
    {
        return mChunks.empty();
    }

    /** @brief Returns the heap bytes of all chunks. */
    [[nodiscard]] size_t MemoryBytes() const noexcept;

private:
    struct Chunk
    {
        value_type *data = nullptr;
        uint32_t used = 0;
        uint32_t capacity = 0;
    };

    /** @brief Frees every chunk. */
    void Release() noexcept;

    std::vector<Chunk> mChunks;
};

/** @brief Required size of CompactLineFields. */
inline constexpr size_t COMPACT_LINE_FIELDS_EXPECTED_BYTES = 16;
static_assert(sizeof(CompactLineFields) == COMPACT_LINE_FIELDS_EXPECTED_BYTES, "CompactLineFields must stay 16 bytes");

} // namespace loglib
