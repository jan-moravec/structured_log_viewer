#include "loglib/key_index.hpp"

#include "transparent_string_hash.hpp"

#include <tsl/robin_map.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <deque>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace loglib
{
using detail::TransparentStringEqual;
using detail::TransparentStringHash;

struct KeyIndex::Impl
{
    /// Power-of-two shard count: hash-low-bits AND `kShardMask` picks a shard.
    static constexpr size_t kShardCount = 16;
    static constexpr size_t kShardMask = kShardCount - 1;

    struct Shard
    {
        tsl::robin_map<std::string, KeyId, TransparentStringHash, TransparentStringEqual> map;
        mutable std::shared_mutex mutex;
    };
    std::array<Shard, kShardCount> shards;

    /// KeyId -> owning string. `deque` keeps inserted strings pointer-stable,
    /// which `KeyOf`'s contract relies on.
    std::deque<std::string> reverse;

    /// High-water KeyId, stored separately so `Size()` is lock-free.
    std::atomic<size_t> size{0};

    /// Serialises `reverse.emplace_back` against `KeyOf` readers.
    mutable std::mutex reverseMutex;

    static size_t ShardIndex(std::string_view key) noexcept
    {
        return std::hash<std::string_view>{}(key)&kShardMask;
    }
};

KeyIndex::KeyIndex() : mImpl(std::make_unique<Impl>())
{
}

KeyIndex::~KeyIndex() = default;

KeyIndex::KeyIndex(KeyIndex &&) noexcept = default;
KeyIndex &KeyIndex::operator=(KeyIndex &&) noexcept = default;

KeyId KeyIndex::GetOrInsert(std::string_view key)
{
#ifdef LOGLIB_KEY_INDEX_INSTRUMENTATION
    sGetOrInsertCallCount.fetch_add(1, std::memory_order_relaxed);
#endif

    Impl::Shard &shard = mImpl->shards[Impl::ShardIndex(key)];

    {
        std::shared_lock<std::shared_mutex> lock(shard.mutex);
        if (auto it = shard.map.find(key); it != shard.map.end())
        {
            return it->second;
        }
    }

    std::scoped_lock locks(shard.mutex, mImpl->reverseMutex);

    if (auto it = shard.map.find(key); it != shard.map.end())
    {
        return it->second;
    }

    // Append to `reverse` first so the string address is stable before any
    // other thread can observe the new id.
    const KeyId id = static_cast<KeyId>(mImpl->reverse.size());
    mImpl->reverse.emplace_back(key);
    shard.map.emplace(mImpl->reverse.back(), id);

    mImpl->size.store(mImpl->reverse.size(), std::memory_order_release);
    return id;
}

KeyId KeyIndex::Find(std::string_view key) const
{
#ifdef LOGLIB_KEY_INDEX_INSTRUMENTATION
    sFindCallCount.fetch_add(1, std::memory_order_relaxed);
#endif

    const Impl::Shard &shard = mImpl->shards[Impl::ShardIndex(key)];
    std::shared_lock<std::shared_mutex> lock(shard.mutex);
    if (auto it = shard.map.find(key); it != shard.map.end())
    {
        return it->second;
    }
    return kInvalidKeyId;
}

std::string_view KeyIndex::KeyOf(KeyId id) const
{
    // Append-only, pointer-stable deque: lock-free read after the KeyId has
    // been observed (release-store on `size` in GetOrInsert).
    return mImpl->reverse[id];
}

size_t KeyIndex::Size() const
{
    return mImpl->size.load(std::memory_order_acquire);
}

std::vector<std::string> KeyIndex::SortedKeys() const
{
    std::scoped_lock lock(mImpl->reverseMutex);
    std::vector<std::string> result(mImpl->reverse.begin(), mImpl->reverse.end());
    std::sort(result.begin(), result.end());
    return result;
}

#ifdef LOGLIB_KEY_INDEX_INSTRUMENTATION
std::atomic<std::size_t> KeyIndex::sGetOrInsertCallCount{0};
std::atomic<std::size_t> KeyIndex::sFindCallCount{0};

void KeyIndex::ResetInstrumentationCounters() noexcept
{
    sGetOrInsertCallCount.store(0, std::memory_order_relaxed);
    sFindCallCount.store(0, std::memory_order_relaxed);
}

std::size_t KeyIndex::LoadGetOrInsertCount() noexcept
{
    return sGetOrInsertCallCount.load(std::memory_order_relaxed);
}

std::size_t KeyIndex::LoadFindCount() noexcept
{
    return sFindCallCount.load(std::memory_order_relaxed);
}
#endif

} // namespace loglib
