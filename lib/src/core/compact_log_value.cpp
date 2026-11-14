#include "loglib/core/compact_log_value.hpp"

#include "loglib/core/enum_dictionary.hpp"
#include "loglib/core/line_source.hpp"
#include "loglib/core/log_line.hpp"

#include <algorithm>
#include <bit>
#include <cassert>
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
        if (!source->SlicesOutliveRows())
        {
            // A stream row's bytes die with the row when it is evicted, so the caller gets a copy.
            return LogValue{std::string(bytes)};
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

} // namespace

CompactLineFields::CompactLineFields(CompactLineFields &&other) noexcept
    : mData(other.mData), mMeta(other.mMeta)
{
    other.mData = nullptr;
    other.mMeta = 0;
}

CompactLineFields &CompactLineFields::operator=(CompactLineFields &&other) noexcept
{
    if (this != &other)
    {
        Clear();
        mData = other.mData;
        mMeta = other.mMeta;
        other.mData = nullptr;
        other.mMeta = 0;
    }
    return *this;
}

CompactLineFields::~CompactLineFields()
{
    Clear();
}

void CompactLineFields::Clear() noexcept
{
    if ((mMeta & OWNED_BIT) != 0)
    {
        DeallocatePairs(mData);
    }
    mData = nullptr;
    mMeta &= ~(SIZE_MASK | OWNED_BIT);
}

CompactLineFields::value_type *CompactLineFields::AllocateOwned(uint32_t count)
{
    assert(count <= MAX_FIELDS);
    Clear();
    if (count == 0)
    {
        return nullptr;
    }
    mData = AllocatePairs(count);
    mMeta = (mMeta & ~(SIZE_MASK | OWNED_BIT)) | count | OWNED_BIT;
    return mData;
}

void CompactLineFields::Borrow(value_type *data, uint32_t count) noexcept
{
    assert(count <= MAX_FIELDS);
    Clear();
    if (count == 0)
    {
        return;
    }
    mData = data;
    mMeta = (mMeta & ~(SIZE_MASK | OWNED_BIT)) | count;
}

void CompactLineFields::AssignSorted(const value_type *values, uint32_t count)
{
    value_type *fresh = AllocatePairs(count);
    if (count > 0)
    {
        std::uninitialized_copy_n(values, count, fresh);
    }
    Clear();
    mData = fresh;
    mMeta = (mMeta & ~(SIZE_MASK | OWNED_BIT)) | count | (count > 0 ? OWNED_BIT : 0U);
}

void CompactLineFields::Insert(uint32_t position, CompactLogValue field)
{
    const uint32_t size = Size();
    assert(size < MAX_FIELDS);
    value_type *fresh = AllocatePairs(size + 1U);
    std::uninitialized_copy_n(mData, position, fresh);
    std::construct_at(fresh + position, field);
    std::uninitialized_copy_n(mData + position, size - position, fresh + position + 1U);
    Clear();
    mData = fresh;
    mMeta = (mMeta & ~(SIZE_MASK | OWNED_BIT)) | (size + 1U) | OWNED_BIT;
}

void CompactLineFields::Set(uint32_t position, CompactLogValue field) noexcept
{
    if (position < Size())
    {
        mData[position] = field;
    }
}

size_t CompactLineFields::OwnedMemoryBytes() const noexcept
{
    return (mMeta & OWNED_BIT) != 0 ? static_cast<size_t>(Size()) * FIELD_BYTES : 0U;
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
