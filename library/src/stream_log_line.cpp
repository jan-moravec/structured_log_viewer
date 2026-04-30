#include "loglib/stream_log_line.hpp"

#include <algorithm>
#include <cassert>
#include <stdexcept>
#include <string>
#include <utility>

namespace loglib
{

StreamLineReference::StreamLineReference(std::string sourceName, std::string rawLine, size_t lineId)
    : mPath(std::move(sourceName)), mRawLine(std::move(rawLine)), mLineNumber(lineId)
{
}

const std::filesystem::path &StreamLineReference::GetPath() const noexcept
{
    return mPath;
}

size_t StreamLineReference::GetLineNumber() const noexcept
{
    return mLineNumber;
}

const std::string &StreamLineReference::GetLine() const noexcept
{
    return mRawLine;
}

void StreamLineReference::ShiftLineNumber(size_t delta) noexcept
{
    mLineNumber += delta;
}

namespace
{

/// Promote any `string_view` payload into an owned `std::string` so the
/// resulting `LogValue` is self-contained. `TailingFileSource` does not
/// keep its source bytes alive across the GUI hand-off (PRD §7 *No mmap
/// on the tail*); accepting a view here would be a use-after-free trap.
LogValue ToOwningLogValue(LogValue value)
{
    if (auto *view = std::get_if<std::string_view>(&value))
    {
        return LogValue{std::string(*view)};
    }
    return value;
}

} // namespace

StreamLogLine::StreamLogLine(
    std::vector<std::pair<KeyId, LogValue>> sortedValues, const KeyIndex &keys, StreamLineReference fileReference
)
    : mKeys(&keys), mFileReference(std::move(fileReference))
{
#ifndef NDEBUG
    assert(std::is_sorted(sortedValues.begin(), sortedValues.end(), [](const auto &a, const auto &b) {
        return a.first < b.first;
    }));
#endif
    mValues.reserve(sortedValues.size());
    for (auto &entry : sortedValues)
    {
        mValues.emplace_back(entry.first, ToOwningLogValue(std::move(entry.second)));
    }
}

StreamLogLine::StreamLogLine(const LogMap &values, KeyIndex &keys, StreamLineReference fileReference)
    : mKeys(&keys), mFileReference(std::move(fileReference))
{
    mValues.reserve(values.size());
    for (const auto &[key, value] : values)
    {
        mValues.emplace_back(keys.GetOrInsert(key), ToOwningLogValue(value));
    }
    std::sort(mValues.begin(), mValues.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
}

const LogValue *StreamLogLine::FindValue(KeyId id) const noexcept
{
    for (const auto &entry : mValues)
    {
        if (entry.first == id)
        {
            return &entry.second;
        }
        if (entry.first > id)
        {
            return nullptr;
        }
    }
    return nullptr;
}

LogValue *StreamLogLine::FindValueMutable(KeyId id) noexcept
{
    for (auto &entry : mValues)
    {
        if (entry.first == id)
        {
            return &entry.second;
        }
        if (entry.first > id)
        {
            return nullptr;
        }
    }
    return nullptr;
}

LogValue StreamLogLine::GetValue(KeyId id) const
{
    if (const LogValue *value = FindValue(id))
    {
        return *value;
    }
    return LogValue{std::monostate{}};
}

LogValue StreamLogLine::GetValue(const std::string &key) const
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

void StreamLogLine::SetValue(KeyId id, LogValue value)
{
    LogValue owning = ToOwningLogValue(std::move(value));

    auto it = std::lower_bound(
        mValues.begin(),
        mValues.end(),
        id,
        [](const std::pair<KeyId, LogValue> &lhs, KeyId rhs) { return lhs.first < rhs; }
    );
    if (it != mValues.end() && it->first == id)
    {
        it->second = std::move(owning);
        return;
    }
    mValues.emplace(it, id, std::move(owning));
}

void StreamLogLine::SetValue(const std::string &key, LogValue value)
{
    if (mKeys == nullptr)
    {
        throw std::runtime_error("StreamLogLine::SetValue(string): KeyIndex back-pointer is unset");
    }
    const KeyId id = mKeys->Find(key);
    if (id == kInvalidKeyId)
    {
        throw std::runtime_error(
            "StreamLogLine::SetValue(string): key '" + key + "' is not registered in the KeyIndex"
        );
    }
    SetValue(id, std::move(value));
}

std::vector<std::string> StreamLogLine::GetKeys() const
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

const std::vector<std::pair<KeyId, LogValue>> &StreamLogLine::IndexedValues() const noexcept
{
    return mValues;
}

LogMap StreamLogLine::Values() const
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

void StreamLogLine::RebindKeys(const KeyIndex &keys) noexcept
{
    mKeys = &keys;
}

const KeyIndex &StreamLogLine::Keys() const noexcept
{
    return *mKeys;
}

const StreamLineReference &StreamLogLine::FileReference() const noexcept
{
    return mFileReference;
}

StreamLineReference &StreamLogLine::FileReference() noexcept
{
    return mFileReference;
}

size_t StreamLogLine::ValueCount() const noexcept
{
    return mValues.size();
}

size_t StreamLogLine::OwnedMemoryBytes() const noexcept
{
    size_t total = mValues.capacity() * sizeof(std::pair<KeyId, LogValue>);
    for (const auto &entry : mValues)
    {
        if (const auto *owned = std::get_if<std::string>(&entry.second))
        {
            total += owned->capacity();
        }
    }
    return total;
}

} // namespace loglib
