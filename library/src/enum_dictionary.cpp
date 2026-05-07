#include "loglib/enum_dictionary.hpp"

#include <utility>

namespace loglib
{

EnumValueId EnumDictionary::Find(std::string_view bytes) const noexcept
{
    // `std::unordered_map<std::string, ...>::find(string_view)` requires
    // a heterogeneous comparator (C++20+) which the default specialisation
    // does not provide; the dictionary is small (<= MAX_ENUM_VALUES) so a
    // linear scan over `mValues` is cheaper than constructing a temporary
    // `std::string` for the lookup.
    for (size_t i = 0; i < mValues.size(); ++i)
    {
        if (mValues[i] == bytes)
        {
            return static_cast<EnumValueId>(i);
        }
    }
    return INVALID_ENUM_VALUE_ID;
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
    mValues.emplace_back(bytes);
    mIndex.emplace(mValues.back(), id);
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
    mValues.clear();
    mIndex.clear();
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

EnumDictionary &EnumDictionaryRegistry::GetOrInsert(KeyId key)
{
    if (auto it = mDictionaries.find(key); it != mDictionaries.end())
    {
        return *it->second;
    }
    auto dict = std::make_unique<EnumDictionary>();
    EnumDictionary *raw = dict.get();
    mDictionaries.emplace(key, std::move(dict));
    mIndex[key] = raw;
    return *raw;
}

void EnumDictionaryRegistry::Alias(KeyId canonical, KeyId alias)
{
    if (canonical == alias)
    {
        return;
    }
    const auto canonicalIt = mDictionaries.find(canonical);
    if (canonicalIt == mDictionaries.end())
    {
        return;
    }
    mIndex[alias] = canonicalIt->second.get();
}

void EnumDictionaryRegistry::Erase(KeyId canonical) noexcept
{
    const auto canonicalIt = mDictionaries.find(canonical);
    if (canonicalIt == mDictionaries.end())
    {
        return;
    }
    EnumDictionary *raw = canonicalIt->second.get();
    // Drop every alias pointing at this dictionary. Walk the index map
    // because aliases are not tracked back-pointer-style.
    for (auto it = mIndex.begin(); it != mIndex.end();)
    {
        if (it->second == raw)
        {
            it = mIndex.erase(it);
        }
        else
        {
            ++it;
        }
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
    for (const auto &[key, dict] : mDictionaries)
    {
        bytes += sizeof(KeyId) + sizeof(EnumDictionary);
        if (dict)
        {
            for (const std::string &value : dict->Values())
            {
                bytes += sizeof(std::string) + value.capacity();
            }
        }
    }
    bytes += mIndex.size() * (sizeof(KeyId) + sizeof(EnumDictionary *));
    return bytes;
}

} // namespace loglib
