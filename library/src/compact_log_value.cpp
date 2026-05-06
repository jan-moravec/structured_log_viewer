#include "loglib/internal/compact_log_value.hpp"

#include "loglib/line_source.hpp"
#include "loglib/log_file.hpp"
#include "loglib/log_line.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace loglib::internal
{

CompactLogValue CompactLogValue::MakeMonostate() noexcept
{
    return CompactLogValue{};
}

CompactLogValue CompactLogValue::MakeMmapSlice(uint64_t offset, uint32_t length) noexcept
{
    CompactLogValue v;
    v.payload = offset;
    v.aux = length;
    v.tag = CompactTag::MmapSlice;
    return v;
}

CompactLogValue CompactLogValue::MakeOwnedString(uint64_t offset, uint32_t length) noexcept
{
    CompactLogValue v;
    v.payload = offset;
    v.aux = length;
    v.tag = CompactTag::OwnedString;
    return v;
}

CompactLogValue CompactLogValue::MakeInt64(int64_t value) noexcept
{
    CompactLogValue v;
    v.payload = static_cast<uint64_t>(value);
    v.tag = CompactTag::Int64;
    return v;
}

CompactLogValue CompactLogValue::MakeUint64(uint64_t value) noexcept
{
    CompactLogValue v;
    v.payload = value;
    v.tag = CompactTag::Uint64;
    return v;
}

CompactLogValue CompactLogValue::MakeDouble(double value) noexcept
{
    CompactLogValue v;
    v.payload = std::bit_cast<uint64_t>(value);
    v.tag = CompactTag::Double;
    return v;
}

CompactLogValue CompactLogValue::MakeBool(bool value) noexcept
{
    CompactLogValue v;
    v.payload = value ? 1U : 0U;
    v.tag = CompactTag::Bool;
    return v;
}

CompactLogValue CompactLogValue::MakeTimestamp(TimeStamp value) noexcept
{
    CompactLogValue v;
    v.payload = static_cast<uint64_t>(value.time_since_epoch().count());
    v.tag = CompactTag::Timestamp;
    return v;
}

LogValue CompactLogValue::Materialise(const LineSource *source, size_t lineId) const
{
    switch (tag)
    {
    case CompactTag::Monostate:
        return LogValue{std::monostate{}};
    case CompactTag::MmapSlice:
    {
        if (source == nullptr)
        {
            return LogValue{std::monostate{}};
        }
        const std::string_view bytes = source->ResolveMmapBytes(payload, aux, lineId);
        if (bytes.empty() && aux != 0)
        {
            return LogValue{std::monostate{}};
        }
        return LogValue{bytes};
    }
    case CompactTag::OwnedString:
    {
        if (source == nullptr)
        {
            return LogValue{std::string{}};
        }
        const std::string_view bytes = source->ResolveOwnedBytes(payload, aux, lineId);
        // Materialise as `std::string` so the caller owns the bytes — a
        // stream source's per-line arena can be evicted later, but a
        // returned `std::string` is independent of source lifetime.
        return LogValue{std::string(bytes)};
    }
    case CompactTag::Int64:
        return LogValue{static_cast<int64_t>(payload)};
    case CompactTag::Uint64:
        return LogValue{payload};
    case CompactTag::Double:
        return LogValue{std::bit_cast<double>(payload)};
    case CompactTag::Bool:
        return LogValue{payload != 0};
    case CompactTag::Timestamp:
        return LogValue{TimeStamp{std::chrono::microseconds{static_cast<int64_t>(payload)}}};
    }
    return LogValue{std::monostate{}};
}

CompactLogValue ToCompactLogValue(
    const LogValue &value, std::string &ownedStringArena, const char *fileBegin, size_t fileSize
)
{
    return std::visit(
        [&](const auto &alt) -> CompactLogValue {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, std::monostate>)
            {
                return CompactLogValue::MakeMonostate();
            }
            else if constexpr (std::is_same_v<T, std::string_view>)
            {
                // Prefer the mmap-slice fast path when the view points
                // inside the supplied file's bytes; otherwise it would
                // dangle as soon as the caller's source buffer goes away,
                // so copy into the arena.
                if (fileBegin != nullptr && alt.data() >= fileBegin && alt.data() + alt.size() <= fileBegin + fileSize)
                {
                    const auto offset = static_cast<uint64_t>(alt.data() - fileBegin);
                    return CompactLogValue::MakeMmapSlice(offset, static_cast<uint32_t>(alt.size()));
                }
                const uint64_t offset = ownedStringArena.size();
                ownedStringArena.append(alt.data(), alt.size());
                return CompactLogValue::MakeOwnedString(offset, static_cast<uint32_t>(alt.size()));
            }
            else if constexpr (std::is_same_v<T, std::string>)
            {
                const uint64_t offset = ownedStringArena.size();
                ownedStringArena.append(alt.data(), alt.size());
                return CompactLogValue::MakeOwnedString(offset, static_cast<uint32_t>(alt.size()));
            }
            else if constexpr (std::is_same_v<T, int64_t>)
            {
                return CompactLogValue::MakeInt64(alt);
            }
            else if constexpr (std::is_same_v<T, uint64_t>)
            {
                return CompactLogValue::MakeUint64(alt);
            }
            else if constexpr (std::is_same_v<T, double>)
            {
                return CompactLogValue::MakeDouble(alt);
            }
            else if constexpr (std::is_same_v<T, bool>)
            {
                return CompactLogValue::MakeBool(alt);
            }
            else if constexpr (std::is_same_v<T, TimeStamp>)
            {
                return CompactLogValue::MakeTimestamp(alt);
            }
            else
            {
                static_assert(std::is_same_v<T, void>, "non-exhaustive visitor");
                return CompactLogValue::MakeMonostate();
            }
        },
        value
    );
}

void RebaseOwnedStringOffsets(std::pair<KeyId, CompactLogValue> *values, size_t valueCount, uint64_t delta) noexcept
{
    if (delta == 0)
    {
        return;
    }
    for (size_t i = 0; i < valueCount; ++i)
    {
        if (values[i].second.tag == CompactTag::OwnedString)
        {
            values[i].second.payload += delta;
        }
    }
}

namespace
{

/// Allocator used by `CompactLineFields`. The pair type is trivially
/// copyable and trivially destructible (KeyId is `uint32_t`,
/// CompactLogValue is a POD), so we can use raw `operator new`/`delete`
/// to skip the per-element ctor/dtor that `std::vector` would run.
constexpr size_t PAIR_BYTES = sizeof(std::pair<KeyId, CompactLogValue>);

// `CompactLineFields` allocates with raw `::operator new` and frees
// with raw `::operator delete`, so the value type must be trivially
// destructible (no per-slot dtor pass). `std::pair`'s trivial-copyable
// status differs across stdlibs — libstdc++ defaults its copy
// assignment when both members are trivially copyable, libc++ does not
// — so we deliberately do not assert that here. The typed
// `std::uninitialized_copy_n` / `std::copy_n` / `std::copy_backward`
// calls below are correct regardless and lower to the same memcpy /
// memmove the previous direct-`memcpy` code emitted under -O2 (without
// tripping -Wclass-memaccess on libstdc++).
static_assert(
    std::is_trivially_destructible_v<std::pair<KeyId, CompactLogValue>>,
    "CompactLineFields uses raw operator new/delete; slot pairs must have a trivial destructor"
);
// Trivial copy construction is what makes `std::construct_at` collapse
// into a memcpy under optimisation. Without this, the `EmplaceBack` /
// `Insert` paths below (which `construct_at` into raw bytes returned
// by `::operator new`) would still be correct but unnecessarily costly.
static_assert(
    std::is_trivially_copy_constructible_v<std::pair<KeyId, CompactLogValue>>,
    "CompactLineFields::EmplaceBack constructs into raw memory; pair must be trivially copy-constructible"
);

std::pair<KeyId, CompactLogValue> *AllocatePairs(uint32_t capacity)
{
    if (capacity == 0)
    {
        return nullptr;
    }
    void *raw = ::operator new(static_cast<size_t>(capacity) * PAIR_BYTES);
    return static_cast<std::pair<KeyId, CompactLogValue> *>(raw);
}

void DeallocatePairs(std::pair<KeyId, CompactLogValue> *data) noexcept
{
    if (data != nullptr)
    {
        ::operator delete(data);
    }
}

uint32_t GrowCapacity(uint32_t current, uint32_t needed) noexcept
{
    // Geometric growth (×1.5 rounded up). Matches `std::vector`'s
    // amortised cost while keeping capacity tighter than ×2.
    uint32_t target = current == 0 ? 4U : current + (current / 2U) + 1U;
    if (target < needed)
    {
        target = needed;
    }
    return target;
}

} // namespace

CompactLineFields::CompactLineFields(uint32_t initialCapacity)
    : mData(AllocatePairs(initialCapacity)), mSize(0), mCapacity(initialCapacity)
{
}

CompactLineFields::CompactLineFields(CompactLineFields &&other) noexcept
    : mData(other.mData), mSize(other.mSize), mCapacity(other.mCapacity)
{
    other.mData = nullptr;
    other.mSize = 0;
    other.mCapacity = 0;
}

CompactLineFields &CompactLineFields::operator=(CompactLineFields &&other) noexcept
{
    if (this != &other)
    {
        DeallocatePairs(mData);
        mData = other.mData;
        mSize = other.mSize;
        mCapacity = other.mCapacity;
        other.mData = nullptr;
        other.mSize = 0;
        other.mCapacity = 0;
    }
    return *this;
}

CompactLineFields::~CompactLineFields()
{
    DeallocatePairs(mData);
}

void CompactLineFields::Reserve(uint32_t capacity)
{
    if (capacity <= mCapacity)
    {
        return;
    }
    auto *fresh = AllocatePairs(capacity);
    if (mSize > 0)
    {
        std::uninitialized_copy_n(mData, mSize, fresh);
    }
    DeallocatePairs(mData);
    mData = fresh;
    mCapacity = capacity;
}

void CompactLineFields::AssignSorted(const value_type *values, uint32_t count)
{
    const bool reused = (count <= mCapacity);
    if (!reused)
    {
        DeallocatePairs(mData);
        mData = AllocatePairs(count);
        mCapacity = count;
    }
    if (count > 0)
    {
        // When reusing the existing buffer the slots are previously-
        // constructed (assigned by `EmplaceBack` / `Insert` / earlier
        // `AssignSorted`); on a fresh allocation they are raw bytes.
        // For trivially-copyable pairs the two operations collapse to
        // the same memcpy under -O2.
        if (reused)
        {
            std::copy_n(values, count, mData);
        }
        else
        {
            std::uninitialized_copy_n(values, count, mData);
        }
    }
    mSize = count;
}

void CompactLineFields::AssignSorted(std::vector<value_type> &&values)
{
    AssignSorted(values.data(), static_cast<uint32_t>(values.size()));
}

void CompactLineFields::EmplaceBack(KeyId key, CompactLogValue value)
{
    if (mSize == mCapacity)
    {
        Reserve(GrowCapacity(mCapacity, mSize + 1U));
    }
    // Slot `mData[mSize]` is raw bytes (the buffer comes from
    // `::operator new`, and `Reserve` only `uninitialized_copy_n`s the
    // first `mSize` elements). Use `construct_at` so an object's
    // lifetime is properly begun before we write to it -- assigning to
    // a non-existent object would be UB even for trivially-copyable
    // types. The static_assert above guarantees `construct_at` lowers
    // to a memcpy under optimisation.
    std::construct_at(mData + mSize, key, value);
    ++mSize;
}

void CompactLineFields::Insert(uint32_t position, KeyId key, CompactLogValue value)
{
    if (mSize == mCapacity)
    {
        Reserve(GrowCapacity(mCapacity, mSize + 1U));
    }
    if (position < mSize)
    {
        // Shift `[position, mSize)` one slot to the right. The slot at
        // `mData + mSize` is raw memory, so first construct it from
        // the last live element, then `copy_backward` the remainder
        // (which is now an assignment into already-constructed slots,
        // not a write to raw memory). The compiler lowers the whole
        // sequence to memmove for trivially-copyable pairs.
        std::construct_at(mData + mSize, mData[mSize - 1]);
        if (position + 1 < mSize)
        {
            std::copy_backward(mData + position, mData + mSize - 1, mData + mSize);
        }
        mData[position] = {key, value};
    }
    else
    {
        // Append slot is raw memory; construct the new pair there.
        std::construct_at(mData + mSize, key, value);
    }
    ++mSize;
}

void CompactLineFields::Set(uint32_t position, CompactLogValue value) noexcept
{
    if (position < mSize)
    {
        mData[position].second = value;
    }
}

size_t CompactLineFields::OwnedMemoryBytes() const noexcept
{
    return static_cast<size_t>(mCapacity) * PAIR_BYTES;
}

void CompactLineFields::ShrinkToFit()
{
    if (mCapacity == mSize)
    {
        return;
    }
    if (mSize == 0)
    {
        DeallocatePairs(mData);
        mData = nullptr;
        mCapacity = 0;
        return;
    }
    auto *fresh = AllocatePairs(mSize);
    std::uninitialized_copy_n(mData, mSize, fresh);
    DeallocatePairs(mData);
    mData = fresh;
    mCapacity = mSize;
}

} // namespace loglib::internal
