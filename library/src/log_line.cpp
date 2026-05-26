#include "loglib/log_line.hpp"

#include "loglib/internal/compact_log_value.hpp"
#include "loglib/line_source.hpp"

#include <algorithm>
#include <cassert>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

namespace loglib
{

std::optional<std::string_view> AsStringView(const LogValue &value)
{
    if (const auto *view = std::get_if<std::string_view>(&value))
    {
        return *view;
    }
    if (const auto *owned = std::get_if<std::string>(&value))
    {
        return std::string_view(*owned);
    }
    return std::nullopt;
}

bool HoldsString(const LogValue &value)
{
    return std::holds_alternative<std::string_view>(value) || std::holds_alternative<std::string>(value);
}

LogValue ToOwnedLogValue(const LogValue &value)
{
    if (const auto *view = std::get_if<std::string_view>(&value))
    {
        return std::string(*view);
    }
    return value;
}

bool LogValueEquivalent(const LogValue &lhs, const LogValue &rhs)
{
    // `string_view` and `string` count as the same kind for equality.
    const auto lhsString = AsStringView(lhs);
    const auto rhsString = AsStringView(rhs);
    if (lhsString.has_value() || rhsString.has_value())
    {
        return lhsString.has_value() && rhsString.has_value() && *lhsString == *rhsString;
    }
    return lhs == rhs;
}

std::optional<int64_t> AsEpochMicroseconds(const LogValue &value)
{
    // Slot acceptance set must stay in lockstep with
    // `TimeRangeRowPredicate`: `TimeStamp`, `int64_t`, and `uint64_t`
    // up to `int64_t::max`. Out-of-range `uint64_t` returns `nullopt`
    // rather than wrapping.
    return std::visit(
        [](const auto &alt) -> std::optional<int64_t> {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, TimeStamp>)
            {
                return alt.time_since_epoch().count();
            }
            else if constexpr (std::is_same_v<T, int64_t>)
            {
                return alt;
            }
            else if constexpr (std::is_same_v<T, uint64_t>)
            {
                if (alt <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
                {
                    return static_cast<int64_t>(alt);
                }
                return std::nullopt;
            }
            else
            {
                return std::nullopt;
            }
        },
        value
    );
}

namespace
{

/// Append @p sv to @p source's owned arena and return the corresponding
/// `OwnedString` value.
internal::CompactLogValue PromoteToOwnedString(LineSource &source, size_t lineId, std::string_view sv)
{
    const uint64_t offset = source.AppendOwnedBytes(lineId, sv);
    return internal::CompactLogValue::MakeOwnedString(offset, static_cast<uint32_t>(sv.size()));
}

internal::CompactLogValue MakeCompactFromVariant(LineSource &source, size_t lineId, const LogValue &value)
{
    return std::visit(
        [&](const auto &alt) -> internal::CompactLogValue {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, std::monostate>)
            {
                return internal::CompactLogValue::MakeMonostate();
            }
            else if constexpr (std::is_same_v<T, std::string_view>)
            {
                // MmapSlice when the view aliases stable bytes;
                // otherwise promote to owned.
                const std::span<const char> stable = source.StableBytes();
                if (!stable.empty() && alt.data() >= stable.data() &&
                    alt.data() + alt.size() <= stable.data() + stable.size())
                {
                    const auto offset = static_cast<uint64_t>(alt.data() - stable.data());
                    return internal::CompactLogValue::MakeMmapSlice(offset, static_cast<uint32_t>(alt.size()));
                }
                return PromoteToOwnedString(source, lineId, alt);
            }
            else if constexpr (std::is_same_v<T, std::string>)
            {
                return PromoteToOwnedString(source, lineId, alt);
            }
            else if constexpr (std::is_same_v<T, int64_t>)
            {
                return internal::CompactLogValue::MakeInt64(alt);
            }
            else if constexpr (std::is_same_v<T, uint64_t>)
            {
                return internal::CompactLogValue::MakeUint64(alt);
            }
            else if constexpr (std::is_same_v<T, double>)
            {
                return internal::CompactLogValue::MakeDouble(alt);
            }
            else if constexpr (std::is_same_v<T, bool>)
            {
                return internal::CompactLogValue::MakeBool(alt);
            }
            else if constexpr (std::is_same_v<T, TimeStamp>)
            {
                return internal::CompactLogValue::MakeTimestamp(alt);
            }
            else
            {
                static_assert(std::is_same_v<T, void>, "non-exhaustive visitor!");
                return internal::CompactLogValue::MakeMonostate();
            }
        },
        value
    );
}

} // namespace

// By-value `sortedValues` lets the parser cheaply move its scratch vector into the line; the body
// only reads it, but a const-ref signature would force the hot parser path to keep ownership of an
// already-spent buffer.
LogLine::LogLine(
    std::vector<std::pair<KeyId, LogValue>> sortedValues, // NOLINT(performance-unnecessary-value-param)
    const KeyIndex &keys,
    LineSource &source,
    size_t lineId
)
    : mValues(static_cast<uint32_t>(sortedValues.size())), mKeys(&keys), mSource(&source), mLineId(lineId)
{
#ifndef NDEBUG
    assert(std::ranges::is_sorted(sortedValues, [](const auto &a, const auto &b) { return a.first < b.first; }));
#endif
    for (auto &entry : sortedValues)
    {
        mValues.EmplaceBack(entry.first, MakeCompactFromVariant(source, lineId, entry.second));
    }
}

LogLine::LogLine(
    std::vector<std::pair<KeyId, internal::CompactLogValue>> sortedValues,
    const KeyIndex &keys,
    LineSource &source,
    size_t lineId
)
    : mKeys(&keys), mSource(&source), mLineId(lineId)
{
#ifndef NDEBUG
    assert(std::ranges::is_sorted(sortedValues, [](const auto &a, const auto &b) { return a.first < b.first; }));
#endif
    // Exact-fit copy keeps each LogLine at `size * 16` bytes.
    mValues.AssignSorted(sortedValues.data(), static_cast<uint32_t>(sortedValues.size()));
}

LogLine::LogLine(const LogMap &values, KeyIndex &keys, LineSource &source, size_t lineId)
    : mValues(static_cast<uint32_t>(values.size())), mKeys(&keys), mSource(&source), mLineId(lineId)
{
    std::vector<std::pair<KeyId, internal::CompactLogValue>> staging;
    staging.reserve(values.size());
    for (const auto &[key, value] : values)
    {
        staging.emplace_back(keys.GetOrInsert(key), MakeCompactFromVariant(source, lineId, value));
    }
    std::ranges::sort(staging, [](const auto &a, const auto &b) { return a.first < b.first; });
    mValues.AssignSorted(staging.data(), static_cast<uint32_t>(staging.size()));
}

const internal::CompactLogValue *LogLine::FindCompact(KeyId id) const noexcept
{
    // Linear scan; lines are tiny and sorted, with an early bail.
    const auto *data = mValues.Data();
    const uint32_t size = mValues.Size();
    for (uint32_t i = 0; i < size; ++i)
    {
        if (data[i].first == id)
        {
            return &data[i].second;
        }
        if (data[i].first > id)
        {
            return nullptr;
        }
    }
    return nullptr;
}

internal::CompactLogValue *LogLine::FindCompactMutable(KeyId id) noexcept
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    return const_cast<internal::CompactLogValue *>(std::as_const(*this).FindCompact(id));
}

std::optional<std::string_view> LogLine::PeekStringView(KeyId id) const noexcept
{
    const internal::CompactLogValue *compact = FindCompact(id);
    if (compact == nullptr)
    {
        return std::nullopt;
    }
    return PeekStringView(*compact);
}

std::optional<std::string_view> LogLine::PeekStringView(const internal::CompactLogValue &slot) const noexcept
{
    if (mSource == nullptr)
    {
        return std::nullopt;
    }
    if (slot.tag == internal::CompactTag::MmapSlice)
    {
        const std::string_view bytes = mSource->ResolveMmapBytes(slot.payload, slot.aux, mLineId);
        if (bytes.empty() && slot.aux != 0)
        {
            return std::nullopt;
        }
        return bytes;
    }
    if (slot.tag == internal::CompactTag::OwnedString)
    {
        return mSource->ResolveOwnedBytes(slot.payload, slot.aux, mLineId);
    }
    return std::nullopt;
}

LogValue LogLine::GetValue(KeyId id) const
{
    const internal::CompactLogValue *compact = FindCompact(id);
    if (compact == nullptr)
    {
        return LogValue{std::monostate{}};
    }
    return compact->Materialise(mSource, mLineId, id);
}

LogValue LogLine::GetValue(const std::string &key) const
{
    if (key.empty() || mKeys == nullptr)
    {
        return LogValue{std::monostate{}};
    }
    const KeyId id = mKeys->Find(key);
    if (id == INVALID_KEY_ID)
    {
        return LogValue{std::monostate{}};
    }
    return GetValue(id);
}

void LogLine::SetValue(KeyId id, const LogValue &value)
{
#ifndef NDEBUG
    // Untagged setter is for owned values; views need `LogValueTrustView`.
    assert(!std::holds_alternative<std::string_view>(value));
#endif
    SetValue(id, value, LogValueTrustView{});
}

void LogLine::SetValue(KeyId id, const LogValue &value, LogValueTrustView /*trust*/)
{
    assert(mSource != nullptr);
    SetCompact(id, MakeCompactFromVariant(*mSource, mLineId, value));
}

void LogLine::SetValue(const std::string &key, const LogValue &value)
{
    if (mKeys == nullptr)
    {
        throw std::runtime_error("LogLine::SetValue(string): KeyIndex back-pointer is unset");
    }
    const KeyId id = mKeys->Find(key);
    if (id == INVALID_KEY_ID)
    {
        throw std::runtime_error("LogLine::SetValue(string): key '" + key + "' is not registered in the KeyIndex");
    }
    SetValue(id, value);
}

void LogLine::SetOrReplaceEnumDictRef(KeyId id, EnumValueId vid)
{
    SetCompact(id, internal::CompactLogValue::MakeDictRef(vid));
}

void LogLine::SetCompact(KeyId id, internal::CompactLogValue compact)
{
    auto *data = mValues.Data();
    const uint32_t size = mValues.Size();
    uint32_t lo = 0;
    uint32_t hi = size;
    while (lo < hi)
    {
        const uint32_t mid = lo + ((hi - lo) / 2U);
        if (data[mid].first < id)
        {
            lo = mid + 1U;
        }
        else
        {
            hi = mid;
        }
    }
    if (lo < size && data[lo].first == id)
    {
        mValues.Set(lo, compact);
        return;
    }
    mValues.Insert(lo, id, compact);
}

std::vector<std::string> LogLine::GetKeys() const
{
    std::vector<std::string> keys;
    keys.reserve(mValues.Size());
    if (mKeys == nullptr)
    {
        return keys;
    }
    for (uint32_t i = 0; i < mValues.Size(); ++i)
    {
        keys.emplace_back(mKeys->KeyOf(mValues.Data()[i].first));
    }
    return keys;
}

std::vector<std::pair<KeyId, LogValue>> LogLine::IndexedValues() const
{
    std::vector<std::pair<KeyId, LogValue>> result;
    result.reserve(mValues.Size());
    for (uint32_t i = 0; i < mValues.Size(); ++i)
    {
        const auto &entry = mValues.Data()[i];
        result.emplace_back(entry.first, entry.second.Materialise(mSource, mLineId, entry.first));
    }
    return result;
}

std::span<const std::pair<KeyId, internal::CompactLogValue>> LogLine::CompactValues() const noexcept
{
    return {mValues.Data(), mValues.Size()};
}

LogMap LogLine::Values() const
{
    LogMap snapshot;
    snapshot.reserve(mValues.Size());
    if (mKeys == nullptr)
    {
        return snapshot;
    }
    for (uint32_t i = 0; i < mValues.Size(); ++i)
    {
        const auto &entry = mValues.Data()[i];
        snapshot.emplace(
            std::string(mKeys->KeyOf(entry.first)), entry.second.Materialise(mSource, mLineId, entry.first)
        );
    }
    return snapshot;
}

void LogLine::RebindKeys(const KeyIndex &keys)
{
    mKeys = &keys;
}

const KeyIndex &LogLine::Keys() const
{
    return *mKeys;
}

LineSource *LogLine::Source() const noexcept
{
    return mSource;
}

size_t LogLine::LineId() const noexcept
{
    return mLineId;
}

void LogLine::ShiftLineId(size_t delta) noexcept
{
    mLineId += delta;
}

void LogLine::SetLineId(size_t lineId) noexcept
{
    mLineId = lineId;
}

size_t LogLine::OwnedMemoryBytes() const
{
    return mValues.OwnedMemoryBytes();
}

size_t LogLine::ValueCount() const noexcept
{
    return mValues.Size();
}

void LogLine::RebaseOwnedStringOffsets(uint64_t delta) noexcept
{
    if (delta == 0 || mValues.Empty())
    {
        return;
    }
    internal::RebaseOwnedStringOffsets(mValues.Data(), mValues.Size(), delta);
}

bool LogLine::IsMmapSlice(KeyId id) const noexcept
{
    const internal::CompactLogValue *compact = FindCompact(id);
    return compact != nullptr && compact->tag == internal::CompactTag::MmapSlice;
}

bool LogLine::IsOwnedString(KeyId id) const noexcept
{
    const internal::CompactLogValue *compact = FindCompact(id);
    return compact != nullptr && compact->tag == internal::CompactTag::OwnedString;
}

bool LogLine::IsDictRef(KeyId id) const noexcept
{
    const internal::CompactLogValue *compact = FindCompact(id);
    return compact != nullptr && compact->tag == internal::CompactTag::DictRef;
}

std::optional<EnumValueId> LogLine::GetEnumValueId(KeyId id) const noexcept
{
    const internal::CompactLogValue *compact = FindCompact(id);
    if (compact == nullptr || compact->tag != internal::CompactTag::DictRef)
    {
        return std::nullopt;
    }
    return static_cast<EnumValueId>(static_cast<uint16_t>(compact->payload));
}

} // namespace loglib
