#include "loglib/enum_dictionary.hpp"

#include <algorithm>
#include <cassert>
#include <utility>

namespace loglib
{

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
    // Construct the string in `mValues` first so the address is
    // stable; key the index against a `string_view` over those
    // stable bytes (deque elements never move on push_back). This
    // avoids storing the same bytes twice (once in the value vector,
    // once as the index key) which the old vector + robin_map<string,..>
    // pair did.
    mValues.emplace_back(bytes);
    const std::string_view key{mValues.back()};
    mIndex.emplace(key, id);
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
    // Index references string_views into `mValues`, so clear it
    // before destroying the underlying bytes.
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
    if (auto it = mDictionaries.find(key); it != mDictionaries.end())
    {
        return *it->second.dict;
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
        // Idempotent identity: caller can pass the canonical key as
        // its own alias without it being treated as an error.
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
        // Already pointing at the same dictionary: idempotent. Any
        // other target is a configuration error -- the caller must
        // `Erase` the existing dictionary before reparenting.
        return indexIt->second == raw;
    }
    if (mDictionaries.contains(alias))
    {
        // The alias key has its own canonical entry; aliasing onto
        // `canonical` would orphan it. Caller must `Erase` first.
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
    // O(aliases per column) instead of O(|mIndex|): walk the entry's
    // own alias list rather than scanning every entry in the index.
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
