#pragma once

#include "loglib/enum_dictionary.hpp"
#include "loglib/key_index.hpp"
#include "loglib/log_value.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace loglib
{
class LineSource;
}

namespace loglib::internal
{

/// Discriminator for `CompactLogValue`. Mirrors `LogValue`'s alternatives
/// but stores strings as `(offset, length)` pairs.
enum class CompactTag : uint8_t
{
    Monostate = 0,
    MmapSlice,   ///< payload = offset into `ResolveMmapBytes`, aux = length
    OwnedString, ///< payload = offset into `ResolveOwnedBytes`, aux = length
    DictRef,     ///< payload = `EnumValueId` (`uint16_t`); resolved via `LineSource::EnumDictionaries`
    Int64,
    Uint64,
    Double,
    Bool,
    Timestamp,
};

/// 16-byte tagged union (8 B payload + 4 B aux + 1 B tag + padding).
/// Per-field storage; `LogValue` is materialised on demand.
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

    /// Materialise into a `LogValue`. For `DictRef`, pass `INVALID_KEY_ID`
    /// to materialise as monostate. Null source is safe.
    LogValue Materialise(const LineSource *source, size_t lineId, KeyId keyId = INVALID_KEY_ID) const;
};

/// Per-field storage size invariant. Growing `CompactLogValue` past this
/// inflates every per-line allocation; the static_assert below is the
/// enforcer.
inline constexpr size_t COMPACT_LOG_VALUE_EXPECTED_BYTES = 16;
static_assert(sizeof(CompactLogValue) == COMPACT_LOG_VALUE_EXPECTED_BYTES, "CompactLogValue must stay 16 bytes");

/// Convert a `LogValue` to a `CompactLogValue`. Strings inside
/// `[fileBegin, fileBegin + fileSize)` become `MmapSlice`; others are
/// copied into @p ownedStringArena.
CompactLogValue ToCompactLogValue(
    const LogValue &value, std::string &ownedStringArena, const char *fileBegin = nullptr, size_t fileSize = 0
);

/// Add @p delta to every `OwnedString` payload in @p values.
void RebaseOwnedStringOffsets(std::pair<KeyId, CompactLogValue> *values, size_t valueCount, uint64_t delta) noexcept;

/// Per-line compact field storage: exact-fit heap array of `(KeyId,
/// CompactLogValue)` pairs sorted by KeyId. Pointer+size+capacity = 16 B,
/// avoiding `std::vector`'s reserve overhead on narrow rows.
class CompactLineFields
{
public:
    using value_type = std::pair<KeyId, CompactLogValue>;

    CompactLineFields() = default;

    /// Allocates @p initialCapacity slots without constructing them.
    explicit CompactLineFields(uint32_t initialCapacity);

    CompactLineFields(const CompactLineFields &) = delete;
    CompactLineFields &operator=(const CompactLineFields &) = delete;

    CompactLineFields(CompactLineFields &&other) noexcept;
    CompactLineFields &operator=(CompactLineFields &&other) noexcept;
    ~CompactLineFields();

    [[nodiscard]] uint32_t Size() const noexcept
    {
        return mSize;
    }

    [[nodiscard]] uint32_t Capacity() const noexcept
    {
        return mCapacity;
    }

    [[nodiscard]] bool Empty() const noexcept
    {
        return mSize == 0;
    }

    [[nodiscard]] value_type *Data() noexcept
    {
        return mData;
    }

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

    /// Grow capacity to at least @p capacity. No-op if already large enough.
    void Reserve(uint32_t capacity);

    /// Replace contents with @p values; reuses the buffer when large
    /// enough, else reallocates exact-fit.
    void AssignSorted(const value_type *values, uint32_t count);
    void AssignSorted(std::vector<value_type> &&values);

    /// Append at the end; caller must keep the array sorted.
    void EmplaceBack(KeyId key, CompactLogValue value);

    /// Insert at @p position; O(N) shifts.
    void Insert(uint32_t position, KeyId key, CompactLogValue value);

    /// Replace the value at @p position.
    void Set(uint32_t position, CompactLogValue value) noexcept;

    /// Heap bytes owned (benchmark-only).
    [[nodiscard]] size_t OwnedMemoryBytes() const noexcept;

    void ShrinkToFit();

private:
    value_type *mData = nullptr;
    uint32_t mSize = 0;
    uint32_t mCapacity = 0;
};

/// `CompactLineFields` packs three pointer/size words; growing past this
/// would inflate every per-line allocation and break the cache-friendly
/// layout the parsers rely on. The static_assert below is the enforcer.
inline constexpr size_t COMPACT_LINE_FIELDS_EXPECTED_BYTES = 16;
static_assert(sizeof(CompactLineFields) == COMPACT_LINE_FIELDS_EXPECTED_BYTES, "CompactLineFields must stay 16 bytes");

} // namespace loglib::internal
