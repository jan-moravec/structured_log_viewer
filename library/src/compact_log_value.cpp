#include "loglib/internal/compact_log_value.hpp"

#include "loglib/line_source.hpp"
#include "loglib/log_file.hpp"
#include "loglib/log_line.hpp"

#include <bit>
#include <chrono>
#include <cstring>
#include <string>
#include <string_view>
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
                const auto offset = static_cast<uint64_t>(ownedStringArena.size());
                ownedStringArena.append(alt.data(), alt.size());
                return CompactLogValue::MakeOwnedString(offset, static_cast<uint32_t>(alt.size()));
            }
            else if constexpr (std::is_same_v<T, std::string>)
            {
                const auto offset = static_cast<uint64_t>(ownedStringArena.size());
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
constexpr size_t kPairBytes = sizeof(std::pair<KeyId, CompactLogValue>);

std::pair<KeyId, CompactLogValue> *AllocatePairs(uint32_t capacity)
{
    if (capacity == 0)
    {
        return nullptr;
    }
    void *raw = ::operator new(static_cast<size_t>(capacity) * kPairBytes);
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
        std::memcpy(fresh, mData, static_cast<size_t>(mSize) * kPairBytes);
    }
    DeallocatePairs(mData);
    mData = fresh;
    mCapacity = capacity;
}

void CompactLineFields::AssignSorted(const value_type *values, uint32_t count)
{
    if (count > mCapacity)
    {
        DeallocatePairs(mData);
        mData = AllocatePairs(count);
        mCapacity = count;
    }
    if (count > 0)
    {
        std::memcpy(mData, values, static_cast<size_t>(count) * kPairBytes);
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
    mData[mSize] = {key, value};
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
        std::memmove(mData + position + 1, mData + position, static_cast<size_t>(mSize - position) * kPairBytes);
    }
    mData[position] = {key, value};
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
    return static_cast<size_t>(mCapacity) * kPairBytes;
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
    std::memcpy(fresh, mData, static_cast<size_t>(mSize) * kPairBytes);
    DeallocatePairs(mData);
    mData = fresh;
    mCapacity = mSize;
}

} // namespace loglib::internal
