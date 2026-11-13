#include "loglib/core/compact_log_value.hpp"

#include "loglib/core/enum_dictionary.hpp"
#include "loglib/core/line_source.hpp"
#include "loglib/core/log_line.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace loglib
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

CompactLogValue CompactLogValue::MakeDictRef(EnumValueId id) noexcept
{
    CompactLogValue v;
    v.payload = static_cast<uint64_t>(id);
    v.aux = 0;
    v.tag = CompactTag::DictRef;
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

LogValue CompactLogValue::Materialise(const LineSource *source, size_t lineId, KeyId keyId) const
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
        // Copy into `std::string` so the caller owns the bytes (stream
        // arenas may be evicted later).
        return LogValue{std::string(bytes)};
    }
    case CompactTag::DictRef:
    {
        if (source == nullptr || keyId == INVALID_KEY_ID)
        {
            return LogValue{std::monostate{}};
        }
        const EnumDictionaryRegistry *registry = source->EnumDictionaries();
        if (registry == nullptr)
        {
            return LogValue{std::monostate{}};
        }
        const EnumDictionary *dict = registry->Find(keyId);
        if (dict == nullptr)
        {
            return LogValue{std::monostate{}};
        }
        // Dictionary bytes are stable for its lifetime.
        const std::string_view bytes = dict->Resolve(static_cast<EnumValueId>(payload));
        return LogValue{bytes};
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

void RebaseOwnedStringOffsets(CompactLogValue *values, size_t valueCount, uint64_t delta) noexcept
{
    if (delta == 0)
    {
        return;
    }
    for (size_t i = 0; i < valueCount; ++i)
    {
        if (values[i].tag == CompactTag::OwnedString)
        {
            values[i].payload += delta;
        }
    }
}

namespace
{

/// `CompactLineFields` uses raw `::operator new` to avoid per-element
/// ctor/dtor work; a field must therefore be trivially destructible and
/// trivially copy-constructible.
constexpr size_t FIELD_BYTES = sizeof(CompactLogValue);

static_assert(
    std::is_trivially_destructible_v<CompactLogValue>, "CompactLineFields fields must be trivially destructible"
);
static_assert(
    std::is_trivially_copy_constructible_v<CompactLogValue>,
    "CompactLineFields fields must be trivially copy-constructible"
);

CompactLogValue *AllocatePairs(uint32_t capacity)
{
    if (capacity == 0)
    {
        return nullptr;
    }
    void *raw = ::operator new(static_cast<size_t>(capacity) * FIELD_BYTES);
    return static_cast<CompactLogValue *>(raw);
}

void DeallocatePairs(CompactLogValue *data) noexcept
{
    if (data != nullptr)
    {
        ::operator delete(data);
    }
}

uint32_t GrowCapacity(uint32_t current, uint32_t needed) noexcept
{
    // ×1.5 geometric growth.
    uint32_t target = current == 0 ? 4U : current + (current / 2U) + 1U;
    target = std::max(target, needed);
    return target;
}

} // namespace

CompactLineFields::CompactLineFields(uint32_t initialCapacity)
    : mData(AllocatePairs(initialCapacity)), mCapacity(initialCapacity)
{
}

CompactLineFields CompactLineFields::Borrow(value_type *data, uint32_t count) noexcept
{
    CompactLineFields fields;
    fields.mData = count == 0 ? nullptr : data;
    fields.mSize = count;
    return fields;
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
        if (mCapacity != 0)
        {
            DeallocatePairs(mData);
        }
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
    if (mCapacity != 0)
    {
        DeallocatePairs(mData);
    }
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
    if (mCapacity != 0)
    {
        DeallocatePairs(mData);
    }
    mData = fresh;
    mCapacity = capacity;
}

void CompactLineFields::AssignSorted(const value_type *values, uint32_t count)
{
    const bool reused = (count <= mCapacity);
    if (!reused)
    {
        if (mCapacity != 0)
        {
            DeallocatePairs(mData);
        }
        mData = AllocatePairs(count);
        mCapacity = count;
    }
    if (count > 0)
    {
        // Reused buffer uses copy assignment; fresh uses uninitialized copy.
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
    std::vector<value_type> local = std::move(values);
    AssignSorted(local.data(), static_cast<uint32_t>(local.size()));
}

void CompactLineFields::EmplaceBack(CompactLogValue field)
{
    // A borrowed array has a capacity of zero and takes its first growth here.
    if (mSize >= mCapacity)
    {
        Reserve(GrowCapacity(mCapacity, mSize + 1U));
    }
    // Trailing slot is raw memory; `construct_at` begins object lifetime.
    std::construct_at(mData + mSize, field);
    ++mSize;
}

void CompactLineFields::Insert(uint32_t position, CompactLogValue field)
{
    // A borrowed array has a capacity of zero and takes its first growth here.
    if (mSize >= mCapacity)
    {
        Reserve(GrowCapacity(mCapacity, mSize + 1U));
    }
    if (position < mSize)
    {
        // Shift `[position, mSize)` right by one; construct into the
        // trailing raw slot first, then assign the rest.
        std::construct_at(mData + mSize, mData[mSize - 1]);
        if (position + 1 < mSize)
        {
            std::copy_backward(mData + position, mData + mSize - 1, mData + mSize);
        }
        mData[position] = field;
    }
    else
    {
        std::construct_at(mData + mSize, field);
    }
    ++mSize;
}

void CompactLineFields::Set(uint32_t position, CompactLogValue field) noexcept
{
    if (position < mSize)
    {
        mData[position] = field;
    }
}

size_t CompactLineFields::OwnedMemoryBytes() const noexcept
{
    return static_cast<size_t>(mCapacity) * FIELD_BYTES;
}

namespace
{

/**
 * A chunk holds this many fields, which is 14 KB. It stays under the 16 KB limit of the Windows low-fragmentation heap,
 * whose allocations do not take a global lock; larger chunks made wide rows load slower. The unused tail of the last
 * chunk of a batch is a small part of the batch.
 */
constexpr uint32_t FIELD_ARENA_CHUNK_FIELDS = 900;

} // namespace

FieldArena::~FieldArena()
{
    Release();
}

void FieldArena::Release() noexcept
{
    for (const Chunk &chunk : mChunks)
    {
        DeallocatePairs(chunk.data);
    }
    mChunks.clear();
}

FieldArena::value_type *FieldArena::Allocate(uint32_t count)
{
    if (count == 0)
    {
        return nullptr;
    }
    if (mChunks.empty() || mChunks.back().capacity - mChunks.back().used < count)
    {
        const uint32_t capacity = std::max(count, FIELD_ARENA_CHUNK_FIELDS);
        mChunks.push_back(Chunk{.data = AllocatePairs(capacity), .used = 0, .capacity = capacity});
    }
    Chunk &chunk = mChunks.back();
    value_type *slots = chunk.data + chunk.used;
    chunk.used += count;
    return slots;
}

size_t FieldArena::MemoryBytes() const noexcept
{
    size_t total = 0;
    for (const Chunk &chunk : mChunks)
    {
        total += static_cast<size_t>(chunk.capacity) * FIELD_BYTES;
    }
    return total;
}

} // namespace loglib
