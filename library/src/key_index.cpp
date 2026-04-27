#include "loglib/key_index.hpp"

#include <tsl/robin_map.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace loglib
{

namespace
{

/// Transparent hash for the per-shard `tsl::robin_map<std::string, KeyId, ...>`.
/// All overloads route through `std::hash<std::string_view>` so a `string_view`
/// query lands in the same bucket as the matching owning `std::string`.
struct TransparentStringHash
{
    using is_transparent = void;

    size_t operator()(std::string_view sv) const noexcept
    {
        return std::hash<std::string_view>{}(sv);
    }
    size_t operator()(const std::string &s) const noexcept
    {
        return std::hash<std::string_view>{}(s);
    }
    size_t operator()(const char *s) const noexcept
    {
        return std::hash<std::string_view>{}(std::string_view(s));
    }
};

/// Transparent equality companion for `TransparentStringHash`. Required by
/// `tsl::robin_map`'s heterogeneous-lookup machinery.
struct TransparentStringEqual
{
    using is_transparent = void;

    bool operator()(std::string_view lhs, std::string_view rhs) const noexcept
    {
        return lhs == rhs;
    }
    bool operator()(std::string_view lhs, const std::string &rhs) const noexcept
    {
        return lhs == rhs;
    }
    bool operator()(const std::string &lhs, std::string_view rhs) const noexcept
    {
        return lhs == rhs;
    }
    bool operator()(const std::string &lhs, const std::string &rhs) const noexcept
    {
        return lhs == rhs;
    }
};

} // namespace

struct KeyIndex::Impl
{
    /// Power-of-two shard count so the low bits of a `std::hash<string_view>`
    /// pick a shard via `& kShardMask`.
    static constexpr size_t kShardCount = 16;
    static constexpr size_t kShardMask = kShardCount - 1;

    struct Shard
    {
        tsl::robin_map<std::string, KeyId, TransparentStringHash, TransparentStringEqual> map;
        mutable std::shared_mutex mutex;
    };
    std::array<Shard, kShardCount> shards;

    /// KeyId index -> owning string. `std::deque` is required: it keeps
    /// pointers/views to previously inserted strings stable across appends,
    /// which `KeyOf`'s lifetime contract depends on.
    std::deque<std::string> reverse;

    /// High-water mark of allocated KeyIds. Stored separately so `Size()`
    /// can be read without taking any lock.
    std::atomic<size_t> size{0};

    /// Serialises `reverse.emplace_back` so the deque mutation observed by
    /// `KeyOf(id)` happens-before any other thread reads `id` via the per-
    /// shard map.
    mutable std::mutex reverseMutex;

    static size_t ShardIndex(std::string_view key) noexcept
    {
        return std::hash<std::string_view>{}(key) & kShardMask;
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

    // Append to `reverse` before publishing into the shard map so the entry's
    // address is stable before any other thread can observe the new id.
    const KeyId id = static_cast<KeyId>(mImpl->reverse.size());
    mImpl->reverse.emplace_back(key);
    shard.map.emplace(mImpl->reverse.back(), id);

    // Release-store on size synchronizes-with the acquire-load in Size().
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
    // The reverse deque is append-only and pointer-stable; dereferencing
    // without taking reverseMutex is safe once the KeyId has been observed
    // (release-store on `size` in GetOrInsert).
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
