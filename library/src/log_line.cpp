#include "loglib/log_line.hpp"

#include "loglib/internal/compact_log_value.hpp"
#include "loglib/line_source.hpp"

#include <algorithm>
#include <cassert>
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
    // Treat `string_view` and `string` as the same logical kind.
    const auto lhsString = AsStringView(lhs);
    const auto rhsString = AsStringView(rhs);
    if (lhsString.has_value() || rhsString.has_value())
    {
        return lhsString.has_value() && rhsString.has_value() && *lhsString == *rhsString;
    }
    return lhs == rhs;
}

namespace
{

/// Append @p sv to @p source's owned-bytes arena for @p lineId and
/// return a compact `OwnedString` value pointing at the just-appended
/// bytes. Used by the cold-path ctors and `SetValue`.
detail::CompactLogValue PromoteToOwnedString(LineSource &source, size_t lineId, std::string_view sv)
{
    const uint64_t offset = source.AppendOwnedBytes(lineId, sv);
    return detail::CompactLogValue::MakeOwnedString(offset, static_cast<uint32_t>(sv.size()));
}

detail::CompactLogValue MakeCompactFromVariant(LineSource &source, size_t lineId, const LogValue &value)
{
    return std::visit(
        [&](const auto &alt) -> detail::CompactLogValue {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, std::monostate>)
            {
                return detail::CompactLogValue::MakeMonostate();
            }
            else if constexpr (std::is_same_v<T, std::string_view>)
            {
                // MmapSlice fast path: when the view aliases the
                // source's stable byte range, we can stamp an offset
                // into it and skip the arena copy. Stream sources
                // return an empty stable-bytes span and fall through
                // to `PromoteToOwnedString`.
                const std::span<const char> stable = source.StableBytes();
                if (!stable.empty() && alt.data() >= stable.data() &&
                    alt.data() + alt.size() <= stable.data() + stable.size())
                {
                    const auto offset = static_cast<uint64_t>(alt.data() - stable.data());
                    return detail::CompactLogValue::MakeMmapSlice(offset, static_cast<uint32_t>(alt.size()));
                }
                return PromoteToOwnedString(source, lineId, alt);
            }
            else if constexpr (std::is_same_v<T, std::string>)
            {
                return PromoteToOwnedString(source, lineId, alt);
            }
            else if constexpr (std::is_same_v<T, int64_t>)
            {
                return detail::CompactLogValue::MakeInt64(alt);
            }
            else if constexpr (std::is_same_v<T, uint64_t>)
            {
                return detail::CompactLogValue::MakeUint64(alt);
            }
            else if constexpr (std::is_same_v<T, double>)
            {
                return detail::CompactLogValue::MakeDouble(alt);
            }
            else if constexpr (std::is_same_v<T, bool>)
            {
                return detail::CompactLogValue::MakeBool(alt);
            }
            else if constexpr (std::is_same_v<T, TimeStamp>)
            {
                return detail::CompactLogValue::MakeTimestamp(alt);
            }
            else
            {
                static_assert(std::is_same_v<T, void>, "non-exhaustive visitor!");
                return detail::CompactLogValue::MakeMonostate();
            }
        },
        value
    );
}

} // namespace

LogLine::LogLine(
    std::vector<std::pair<KeyId, LogValue>> sortedValues,
    const KeyIndex &keys,
    LineSource &source,
    size_t lineId
)
    : mValues(static_cast<uint32_t>(sortedValues.size())), mKeys(&keys), mSource(&source), mLineId(lineId)
{
#ifndef NDEBUG
    assert(std::is_sorted(sortedValues.begin(), sortedValues.end(), [](const auto &a, const auto &b) {
        return a.first < b.first;
    }));
#endif
    for (auto &entry : sortedValues)
    {
        mValues.EmplaceBack(entry.first, MakeCompactFromVariant(source, lineId, entry.second));
    }
}

LogLine::LogLine(
    std::vector<std::pair<KeyId, detail::CompactLogValue>> sortedValues,
    const KeyIndex &keys,
    LineSource &source,
    size_t lineId
)
    : mKeys(&keys), mSource(&source), mLineId(lineId)
{
#ifndef NDEBUG
    assert(std::is_sorted(sortedValues.begin(), sortedValues.end(), [](const auto &a, const auto &b) {
        return a.first < b.first;
    }));
#endif
    // Exact-fit copy: drops the temporary vector's `reserve(16)` capacity
    // waste in one go, so each `LogLine` carries only `size * 16` bytes
    // of field storage instead of `capacity * 16`. The temp vector is
    // freed when @p sortedValues goes out of scope.
    mValues.AssignSorted(sortedValues.data(), static_cast<uint32_t>(sortedValues.size()));
}

LogLine::LogLine(const LogMap &values, KeyIndex &keys, LineSource &source, size_t lineId)
    : mValues(static_cast<uint32_t>(values.size())), mKeys(&keys), mSource(&source), mLineId(lineId)
{
    std::vector<std::pair<KeyId, detail::CompactLogValue>> staging;
    staging.reserve(values.size());
    for (const auto &[key, value] : values)
    {
        staging.emplace_back(keys.GetOrInsert(key), MakeCompactFromVariant(source, lineId, value));
    }
    std::sort(staging.begin(), staging.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    mValues.AssignSorted(staging.data(), static_cast<uint32_t>(staging.size()));
}

const detail::CompactLogValue *LogLine::FindCompact(KeyId id) const noexcept
{
    // Linear forward scan: typical line has <= 8 fields and the data is
    // sorted ascending; bailing on `entry.first > id` keeps it branch-light.
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

LogValue LogLine::GetValue(KeyId id) const
{
    const detail::CompactLogValue *compact = FindCompact(id);
    if (compact == nullptr)
    {
        return LogValue{std::monostate{}};
    }
    return compact->Materialise(mSource, mLineId);
}

LogValue LogLine::GetValue(const std::string &key) const
{
    if (key.empty() || mKeys == nullptr)
    {
        return LogValue{std::monostate{}};
    }
    const KeyId id = mKeys->Find(key);
    if (id == kInvalidKeyId)
    {
        return LogValue{std::monostate{}};
    }
    return GetValue(id);
}

void LogLine::SetValue(KeyId id, LogValue value)
{
#ifndef NDEBUG
    // Untagged setter is for owned values; callers passing a `string_view`
    // must use the `LogValueTrustView` overload to declare the lifetime.
    assert(!std::holds_alternative<std::string_view>(value));
#endif
    SetValue(id, std::move(value), LogValueTrustView{});
}

void LogLine::SetValue(KeyId id, LogValue value, LogValueTrustView /*trust*/)
{
    assert(mSource != nullptr);
    detail::CompactLogValue compact = MakeCompactFromVariant(*mSource, mLineId, value);
    auto *data = mValues.Data();
    const uint32_t size = mValues.Size();
    uint32_t lo = 0;
    uint32_t hi = size;
    while (lo < hi)
    {
        const uint32_t mid = lo + (hi - lo) / 2U;
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
        // In-place replacement: hot path for inline timestamp promotion
        // (`tryPromote` swaps the field's string value for a `TimeStamp`).
        mValues.Set(lo, compact);
        return;
    }
    mValues.Insert(lo, id, compact);
}

void LogLine::SetValue(const std::string &key, LogValue value)
{
    if (mKeys == nullptr)
    {
        throw std::runtime_error("LogLine::SetValue(string): KeyIndex back-pointer is unset");
    }
    const KeyId id = mKeys->Find(key);
    if (id == kInvalidKeyId)
    {
        throw std::runtime_error("LogLine::SetValue(string): key '" + key + "' is not registered in the KeyIndex");
    }
    SetValue(id, std::move(value));
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
        result.emplace_back(entry.first, entry.second.Materialise(mSource, mLineId));
    }
    return result;
}

std::span<const std::pair<KeyId, detail::CompactLogValue>> LogLine::CompactValues() const noexcept
{
    return std::span<const std::pair<KeyId, detail::CompactLogValue>>(mValues.Data(), mValues.Size());
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
        snapshot.emplace(std::string(mKeys->KeyOf(entry.first)), entry.second.Materialise(mSource, mLineId));
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
    detail::RebaseOwnedStringOffsets(mValues.Data(), mValues.Size(), delta);
}

bool LogLine::IsMmapSlice(KeyId id) const noexcept
{
    const detail::CompactLogValue *compact = FindCompact(id);
    return compact != nullptr && compact->tag == detail::CompactTag::MmapSlice;
}

bool LogLine::IsOwnedString(KeyId id) const noexcept
{
    const detail::CompactLogValue *compact = FindCompact(id);
    return compact != nullptr && compact->tag == detail::CompactTag::OwnedString;
}

} // namespace loglib
