#include "loglib/log_line.hpp"

#include <algorithm>
#include <cassert>
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

LogLine::LogLine(std::vector<std::pair<KeyId, LogValue>> sortedValues, const KeyIndex &keys, LogFileReference fileReference)
    : mValues(std::move(sortedValues)), mKeys(&keys), mFileReference(std::move(fileReference))
{
#ifndef NDEBUG
    // Precondition: ascending-by-KeyId; `GetValue` and `LogData::Merge` rely on it.
    assert(std::is_sorted(mValues.begin(), mValues.end(), [](const auto &a, const auto &b) {
        return a.first < b.first;
    }));
#endif
}

LogLine::LogLine(const LogMap &values, KeyIndex &keys, LogFileReference fileReference)
    : mKeys(&keys), mFileReference(std::move(fileReference))
{
    mValues.reserve(values.size());
    for (const auto &[key, value] : values)
    {
        mValues.emplace_back(keys.GetOrInsert(key), value);
    }
    std::sort(mValues.begin(), mValues.end(), [](const auto &a, const auto &b) {
        return a.first < b.first;
    });
}

LogValue LogLine::GetValue(KeyId id) const
{
    for (const auto &entry : mValues)
    {
        if (entry.first == id)
        {
            return entry.second;
        }
        if (entry.first > id)
        {
            break;
        }
    }
    return std::monostate{};
}

LogValue LogLine::GetValue(const std::string &key) const
{
    if (key.empty() || mKeys == nullptr)
    {
        return std::monostate{};
    }
    const KeyId id = mKeys->Find(key);
    if (id == kInvalidKeyId)
    {
        return std::monostate{};
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
    auto it = std::lower_bound(mValues.begin(), mValues.end(), id, [](const auto &entry, KeyId target) {
        return entry.first < target;
    });
    if (it != mValues.end() && it->first == id)
    {
        it->second = std::move(value);
        return;
    }
    mValues.emplace(it, id, std::move(value));
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
    keys.reserve(mValues.size());
    if (mKeys == nullptr)
    {
        return keys;
    }
    for (const auto &entry : mValues)
    {
        keys.emplace_back(mKeys->KeyOf(entry.first));
    }
    return keys;
}

std::span<const std::pair<KeyId, LogValue>> LogLine::IndexedValues() const
{
    return std::span<const std::pair<KeyId, LogValue>>(mValues.data(), mValues.size());
}

LogMap LogLine::Values() const
{
    LogMap snapshot;
    snapshot.reserve(mValues.size());
    if (mKeys == nullptr)
    {
        return snapshot;
    }
    for (const auto &entry : mValues)
    {
        snapshot.emplace(std::string(mKeys->KeyOf(entry.first)), entry.second);
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

const LogFileReference &LogLine::FileReference() const
{
    return mFileReference;
}

LogFileReference &LogLine::FileReference()
{
    return mFileReference;
}

} // namespace loglib
