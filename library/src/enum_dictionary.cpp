#include "loglib/enum_dictionary.hpp"

#include <algorithm>
#include <cassert>
#include <utility>

namespace loglib
{

// MSVC's `std::deque` default ctor can theoretically throw; `noexcept` is
// the contract our storage relies on.
// NOLINTNEXTLINE(bugprone-exception-escape)
EnumDictionary::EnumDictionary(uint16_t cap) noexcept
    : mCap(std::clamp<uint16_t>(cap, 1, MAX_ENUM_VALUES))
{
}

EnumValueId EnumDictionary::Find(std::string_view bytes) const noexcept
{
    const auto it = mIndex.find(bytes);
    if (it == mIndex.end())
    {
        return INVALID_ENUM_VALUE_ID;
    }
    return it->second;
}

EnumValueId EnumDictionary::Insert(std::string_view bytes)
{
    if (const EnumValueId existing = Find(bytes); existing != INVALID_ENUM_VALUE_ID)
    {
        return existing;
    }
    if (Full())
    {
        return INVALID_ENUM_VALUE_ID;
    }
    const auto id = static_cast<EnumValueId>(mValues.size());
    // Append first so the bytes have a stable address, then key the index
    // against a view over them. Strong exception guarantee: roll back the
    // append on index-insert failure.
    mValues.emplace_back(bytes);
    try
    {
        const std::string_view key{mValues.back()};
        mIndex.emplace(key, id);
    }
    catch (...)
    {
        mValues.pop_back();
        throw;
    }
    return id;
}

std::string_view EnumDictionary::Resolve(EnumValueId id) const noexcept
{
    const auto idx = static_cast<size_t>(id);
    if (idx >= mValues.size())
    {
        return {};
    }
    return mValues[idx];
}

void EnumDictionary::Clear() noexcept
{
    // Index references string_views into `mValues`, so clear it first.
    mIndex.clear();
    mValues.clear();
}

bool EnumDictionaryRegistry::Contains(KeyId key) const noexcept
{
    return mIndex.contains(key);
}

const EnumDictionary *EnumDictionaryRegistry::Find(KeyId key) const noexcept
{
    const auto it = mIndex.find(key);
    if (it == mIndex.end())
    {
        return nullptr;
    }
    return it->second;
}

EnumDictionary &EnumDictionaryRegistry::GetOrInsert(KeyId key, uint16_t cap)
{
    // `mIndex` covers canonicals + aliases so an alias key returns the
    // canonical's dictionary instead of clobbering the alias mapping.
    if (auto it = mIndex.find(key); it != mIndex.end())
    {
        return *it->second;
    }
    DictionaryEntry entry;
    entry.dict = std::make_unique<EnumDictionary>(cap);
    entry.aliases.push_back(key);
    EnumDictionary *raw = entry.dict.get();
    mDictionaries.emplace(key, std::move(entry));
    mIndex[key] = raw;
    return *raw;
}

bool EnumDictionaryRegistry::Alias(KeyId canonical, KeyId alias)
{
    if (canonical == alias)
    {
        // Identity alias is idempotent.
        return true;
    }
    auto canonicalIt = mDictionaries.find(canonical);
    if (canonicalIt == mDictionaries.end())
    {
        return false;
    }
    EnumDictionary *raw = canonicalIt->second.dict.get();
    if (auto indexIt = mIndex.find(alias); indexIt != mIndex.end())
    {
        // Idempotent if already pointing at the same dict; else a conflict.
        return indexIt->second == raw;
    }
    if (mDictionaries.contains(alias))
    {
        // Alias has its own canonical entry; aliasing would orphan it.
        return false;
    }
    mIndex[alias] = raw;
    canonicalIt.value().aliases.push_back(alias);
    return true;
}

void EnumDictionaryRegistry::Erase(KeyId canonical) noexcept
{
    const auto canonicalIt = mDictionaries.find(canonical);
    if (canonicalIt == mDictionaries.end())
    {
        return;
    }
    for (const KeyId alias : canonicalIt->second.aliases)
    {
        mIndex.erase(alias);
    }
    mDictionaries.erase(canonicalIt);
}

void EnumDictionaryRegistry::Clear() noexcept
{
    mIndex.clear();
    mDictionaries.clear();
}

size_t EnumDictionaryRegistry::EstimatedMemoryBytes() const noexcept
{
    size_t bytes = sizeof(EnumDictionaryRegistry);
    for (const auto &[key, entry] : mDictionaries)
    {
        bytes += sizeof(KeyId) + sizeof(DictionaryEntry);
        bytes += entry.aliases.capacity() * sizeof(KeyId);
        if (entry.dict)
        {
            for (const std::string &value : entry.dict->Values())
            {
                bytes += sizeof(std::string) + value.capacity();
            }
        }
    }
    bytes += mIndex.size() * (sizeof(KeyId) + sizeof(EnumDictionary *));
    return bytes;
}

} // namespace loglib
