#pragma once

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

/// Discriminator for the 16-byte compact internal value representation.
/// Mirrors today's `LogValue` `std::variant` alternatives, but stores
/// strings as `(offset, length)` pairs resolved through the owning
/// `LineSource` (file mmap, per-line stream arena, …) rather than as a
/// 16-byte view or 32/40-byte `std::string`.
enum class CompactTag : uint8_t
{
    Monostate = 0,
    MmapSlice,   ///< payload = byte offset into `LineSource::ResolveMmapBytes`, aux = length
    OwnedString, ///< payload = byte offset into `LineSource::ResolveOwnedBytes`, aux = length
    Int64,
    Uint64,
    Double,
    Bool,
    Timestamp,
};

/// Internal-only 16-byte tagged union. Parsers and `LogLine` use it as the
/// per-field storage type; the public `LogValue` variant is materialised
/// only on `GetValue` / `IndexedValues` access.
///
/// Layout: 8-byte payload + 4-byte aux + 1-byte tag + 3 padding = 16 B.
/// 64-bit `payload` keeps `>4 GB` files working (per-file byte offset).
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
    static CompactLogValue MakeInt64(int64_t value) noexcept;
    static CompactLogValue MakeUint64(uint64_t value) noexcept;
    static CompactLogValue MakeDouble(double value) noexcept;
    static CompactLogValue MakeBool(bool value) noexcept;
    static CompactLogValue MakeTimestamp(TimeStamp value) noexcept;

    /// Materialise into the public `LogValue` variant. @p source provides
    /// the `MmapSlice` and `OwnedString` byte storage via
    /// `LineSource::ResolveMmapBytes` / `ResolveOwnedBytes`; @p lineId
    /// addresses the per-line arena for stream sources (file sources
    /// ignore it). Passing `nullptr` source is safe and yields
    /// `monostate` for the string tags.
    LogValue Materialise(const LineSource *source, size_t lineId) const;
};

static_assert(sizeof(CompactLogValue) == 16, "CompactLogValue must stay 16 bytes (Phase 1 RAM target)");

/// Convert a public `LogValue` to a `CompactLogValue`. Owned strings
/// (`std::string` alternative) are appended to @p ownedStringArena; the
/// `OwnedString` payload is the offset of the just-appended bytes.
/// `string_view` alternatives are stored as `MmapSlice` when @p fileBegin
/// is non-null and the view points inside `[fileBegin, fileBegin + fileSize)`,
/// otherwise they are copied into the arena (cold path; tests/SetValue).
CompactLogValue ToCompactLogValue(
    const LogValue &value, std::string &ownedStringArena, const char *fileBegin = nullptr, size_t fileSize = 0
);

/// In-place add @p delta to every `OwnedString` payload in @p values.
/// Used by `LogData::AppendBatch` and `BufferingSink::OnBatch` when they
/// concatenate a per-batch owned-string arena into the canonical
/// `LogFile::mOwnedStrings`.
void RebaseOwnedStringOffsets(std::pair<KeyId, CompactLogValue> *values, size_t valueCount, uint64_t delta) noexcept;

/// Per-line compact field storage: an exact-fit heap array of
/// `(KeyId, CompactLogValue)` pairs sorted by `KeyId`. Used by `LogLine`
/// in place of `std::vector` so that lines parsed from typical narrow
/// rows (5–6 fields) don't carry the `reserve(16)` capacity waste.
///
/// Layout: pointer (8 B) + size (4 B) + capacity (4 B) = **16 B**, vs
/// 24 B for `std::vector` on 64-bit MSVC. Capacity equals size after
/// `Finalize`, so `OwnedMemoryBytes()` reports the exact heap byte cost.
class CompactLineFields
{
public:
    using value_type = std::pair<KeyId, CompactLogValue>;

    CompactLineFields() = default;

    /// Allocates @p initialCapacity slots without constructing them; intended
    /// for the parser hot path (`reserve`-style preallocation).
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

    /// Replace contents with @p values. Reuses the existing buffer when
    /// `mCapacity >= values.size()`, else reallocates exact-fit.
    void AssignSorted(const value_type *values, uint32_t count);
    void AssignSorted(std::vector<value_type> &&values);

    /// Append @p value at the end (caller-asserted to keep the array
    /// sorted). Grows geometrically.
    void EmplaceBack(KeyId key, CompactLogValue value);

    /// Insert @p value at @p position. Grows geometrically. O(N) shifts.
    void Insert(uint32_t position, KeyId key, CompactLogValue value);

    /// Replace the value at @p position with @p value. Caller must keep
    /// the existing key id at that position.
    void Set(uint32_t position, CompactLogValue value) noexcept;

    /// Heap bytes owned by this storage (capacity, not size). Used by the
    /// memory-footprint benchmark.
    [[nodiscard]] size_t OwnedMemoryBytes() const noexcept;

    /// Drop excess capacity. After call, `mCapacity == mSize`.
    void ShrinkToFit();

private:
    value_type *mData = nullptr;
    uint32_t mSize = 0;
    uint32_t mCapacity = 0;
};

static_assert(sizeof(CompactLineFields) == 16, "CompactLineFields must stay 16 bytes (Phase 2 RAM target)");

} // namespace loglib::internal
