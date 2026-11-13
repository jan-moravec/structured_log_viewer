#include "loglib/core/log_line.hpp"

#include "loglib/core/compact_log_value.hpp"
#include "loglib/core/line_source.hpp"

#include <algorithm>
#include <cassert>
#include <limits>
#include <memory>
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
loglib::CompactLogValue PromoteToOwnedString(LineSource &source, size_t lineId, std::string_view sv)
{
    const uint64_t offset = source.AppendOwnedBytes(lineId, sv);
    return loglib::CompactLogValue::MakeOwnedString(offset, static_cast<uint32_t>(sv.size()));
}

loglib::CompactLogValue MakeCompactFromVariant(LineSource &source, size_t lineId, const LogValue &value)
{
    return std::visit(
        [&](const auto &alt) -> loglib::CompactLogValue {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, std::monostate>)
            {
                return loglib::CompactLogValue::MakeMonostate();
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
                    return loglib::CompactLogValue::MakeMmapSlice(offset, static_cast<uint32_t>(alt.size()));
                }
                return PromoteToOwnedString(source, lineId, alt);
            }
            else if constexpr (std::is_same_v<T, std::string>)
            {
                return PromoteToOwnedString(source, lineId, alt);
            }
            else if constexpr (std::is_same_v<T, int64_t>)
            {
                return loglib::CompactLogValue::MakeInt64(alt);
            }
            else if constexpr (std::is_same_v<T, uint64_t>)
            {
                return loglib::CompactLogValue::MakeUint64(alt);
            }
            else if constexpr (std::is_same_v<T, double>)
            {
                return loglib::CompactLogValue::MakeDouble(alt);
            }
            else if constexpr (std::is_same_v<T, bool>)
            {
                return loglib::CompactLogValue::MakeBool(alt);
            }
            else if constexpr (std::is_same_v<T, TimeStamp>)
            {
                return loglib::CompactLogValue::MakeTimestamp(alt);
            }
            else
            {
                static_assert(std::is_same_v<T, void>, "non-exhaustive visitor!");
                return loglib::CompactLogValue::MakeMonostate();
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
    LineSource &source,
    size_t lineId
)
    : mSource(&source)
{
#ifndef NDEBUG
    assert(std::ranges::is_sorted(sortedValues, [](const auto &a, const auto &b) { return a.first < b.first; }));
#endif
    assert(lineId <= CompactLineFields::MAX_SPARE);
    mValues.SetSpare(lineId);
    auto *fields = mValues.AllocateOwned(static_cast<uint32_t>(sortedValues.size()));
    for (size_t i = 0; i < sortedValues.size(); ++i)
    {
        fields[i] = MakeCompactFromVariant(source, lineId, sortedValues[i].second).WithKey(sortedValues[i].first);
    }
}

LogLine::LogLine(std::vector<loglib::CompactLogValue> sortedValues, LineSource &source, size_t lineId)
    : mSource(&source)
{
#ifndef NDEBUG
    assert(std::ranges::is_sorted(sortedValues, [](const auto &a, const auto &b) { return a.Key() < b.Key(); }));
#endif
    assert(lineId <= CompactLineFields::MAX_SPARE);
    mValues.SetSpare(lineId);
    // Exact-fit copy keeps each LogLine at `size * 16` bytes.
    mValues.AssignSorted(sortedValues.data(), static_cast<uint32_t>(sortedValues.size()));
}

LogLine::LogLine(
    std::span<const loglib::CompactLogValue> sortedValues, FieldArena &arena, LineSource &source, size_t lineId
)
    : mSource(&source)
{
#ifndef NDEBUG
    assert(std::ranges::is_sorted(sortedValues, [](const auto &a, const auto &b) { return a.Key() < b.Key(); }));
#endif
    assert(lineId <= CompactLineFields::MAX_SPARE);
    mValues.SetSpare(lineId);
    const auto count = static_cast<uint32_t>(sortedValues.size());
    auto *slots = arena.Allocate(count);
    std::uninitialized_copy_n(sortedValues.data(), count, slots);
    mValues.Borrow(slots, count);
}

LogLine::LogLine(const LogMap &values, KeyIndex &keys, LineSource &source, size_t lineId)
    : mSource(&source)
{
    assert(lineId <= CompactLineFields::MAX_SPARE);
    mValues.SetSpare(lineId);
    std::vector<loglib::CompactLogValue> staging;
    staging.reserve(values.size());
    for (const auto &[key, value] : values)
    {
        staging.push_back(MakeCompactFromVariant(source, lineId, value).WithKey(keys.GetOrInsert(key)));
    }
    std::ranges::sort(staging, [](const auto &a, const auto &b) { return a.Key() < b.Key(); });
    mValues.AssignSorted(staging.data(), static_cast<uint32_t>(staging.size()));
}

const loglib::CompactLogValue *LogLine::FindCompact(KeyId id) const noexcept
{
    // Linear scan; lines are tiny and sorted, with an early bail.
    const auto *data = mValues.Data();
    const uint32_t size = mValues.Size();
    for (uint32_t i = 0; i < size; ++i)
    {
        if (data[i].Key() == id)
        {
            return &data[i];
        }
        if (data[i].Key() > id)
        {
            return nullptr;
        }
    }
    return nullptr;
}

loglib::CompactLogValue *LogLine::FindCompactMutable(KeyId id) noexcept
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    return const_cast<loglib::CompactLogValue *>(std::as_const(*this).FindCompact(id));
}

std::optional<std::string_view> LogLine::PeekStringView(KeyId id) const noexcept
{
    const loglib::CompactLogValue *compact = FindCompact(id);
    if (compact == nullptr)
    {
        return std::nullopt;
    }
    return PeekStringView(*compact);
}

std::optional<std::string_view> LogLine::PeekStringView(const loglib::CompactLogValue &slot) const noexcept
{
    if (mSource == nullptr)
    {
        return std::nullopt;
    }
    if (slot.tag == loglib::CompactTag::MmapSlice)
    {
        const std::string_view bytes = mSource->ResolveMmapBytes(slot.payload, slot.aux, LineId());
        if (bytes.empty() && slot.aux != 0)
        {
            return std::nullopt;
        }
        return bytes;
    }
    if (slot.tag == loglib::CompactTag::OwnedString)
    {
        return mSource->ResolveOwnedBytes(slot.payload, slot.aux, LineId());
    }
    return std::nullopt;
}

LogValue LogLine::GetValue(KeyId id) const
{
    const loglib::CompactLogValue *compact = FindCompact(id);
    if (compact == nullptr)
    {
        return LogValue{std::monostate{}};
    }
    return compact->Materialise(mSource, LineId(), id);
}

LogValue LogLine::GetValue(const KeyIndex &keys, std::string_view key) const
{
    if (key.empty())
    {
        return LogValue{std::monostate{}};
    }
    const KeyId id = keys.Find(key);
    if (id == INVALID_KEY_ID)
    {
        return LogValue{std::monostate{}};
    }
    return GetValue(id);
}

void LogLine::SetValue(const KeyIndex &keys, std::string_view key, const LogValue &value)
{
    const KeyId id = keys.Find(key);
    if (id == INVALID_KEY_ID)
    {
        throw std::runtime_error(
            "LogLine::SetValue(string): key '" + std::string(key) + "' is not registered in the KeyIndex"
        );
    }
    SetValue(id, value);
}

std::vector<std::string> LogLine::GetKeys(const KeyIndex &keys) const
{
    std::vector<std::string> names;
    names.reserve(mValues.Size());
    for (uint32_t i = 0; i < mValues.Size(); ++i)
    {
        assert(mValues.Data()[i].Key() < keys.Size() && "GetKeys: the key id is not in this index");
        names.emplace_back(keys.KeyOf(mValues.Data()[i].Key()));
    }
    return names;
}

LogMap LogLine::Values(const KeyIndex &keys) const
{
    LogMap snapshot;
    snapshot.reserve(mValues.Size());
    for (uint32_t i = 0; i < mValues.Size(); ++i)
    {
        const auto &entry = mValues.Data()[i];
        assert(entry.Key() < keys.Size() && "Values: the key id is not in this index");
        snapshot.emplace(std::string(keys.KeyOf(entry.Key())), entry.Materialise(mSource, LineId(), entry.Key()));
    }
    return snapshot;
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
    SetCompact(id, MakeCompactFromVariant(*mSource, LineId(), value));
}

void LogLine::SetCompact(KeyId id, loglib::CompactLogValue compact)
{
    auto *data = mValues.Data();
    const uint32_t size = mValues.Size();
    uint32_t lo = 0;
    uint32_t hi = size;
    while (lo < hi)
    {
        const uint32_t mid = lo + ((hi - lo) / 2U);
        if (data[mid].Key() < id)
        {
            lo = mid + 1U;
        }
        else
        {
            hi = mid;
        }
    }
    compact.SetKey(id);
    if (lo < size && data[lo].Key() == id)
    {
        mValues.Set(lo, compact);
        return;
    }
    mValues.Insert(lo, compact);
}

std::vector<std::pair<KeyId, LogValue>> LogLine::IndexedValues() const
{
    std::vector<std::pair<KeyId, LogValue>> result;
    result.reserve(mValues.Size());
    for (uint32_t i = 0; i < mValues.Size(); ++i)
    {
        const auto &entry = mValues.Data()[i];
        result.emplace_back(entry.Key(), entry.Materialise(mSource, LineId(), entry.Key()));
    }
    return result;
}

std::span<const loglib::CompactLogValue> LogLine::CompactValues() const noexcept
{
    return {mValues.Data(), mValues.Size()};
}

LineSource *LogLine::Source() const noexcept
{
    return mSource;
}

size_t LogLine::LineId() const noexcept
{
    return static_cast<size_t>(mValues.Spare());
}

void LogLine::ReleaseFields() noexcept
{
    mValues.Clear();
}

void LogLine::ShiftLineId(size_t delta) noexcept
{
    mValues.SetSpare(mValues.Spare() + delta);
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
    loglib::RebaseOwnedStringOffsets(mValues.Data(), mValues.Size(), delta);
}

bool LogLine::IsDictRef(KeyId id) const noexcept
{
    const loglib::CompactLogValue *compact = FindCompact(id);
    return compact != nullptr && compact->tag == loglib::CompactTag::DictRef;
}

std::optional<EnumValueId> LogLine::GetEnumValueId(KeyId id) const noexcept
{
    const loglib::CompactLogValue *compact = FindCompact(id);
    if (compact == nullptr || compact->tag != loglib::CompactTag::DictRef)
    {
        return std::nullopt;
    }
    return static_cast<EnumValueId>(static_cast<uint16_t>(compact->payload));
}

} // namespace loglib
