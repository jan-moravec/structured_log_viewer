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

/**
 * @brief Transparent hash for the per-shard `tsl::robin_map<std::string, KeyId, ...>`.
 *
 * `tsl::robin_map`'s heterogeneous-lookup overloads are SFINAE-gated on both
 * the hash and the equality declaring an `is_transparent` typedef. We route
 * every overload through `std::hash<std::string_view>` so the bucket a
 * `std::string_view` query lands in is identical to the bucket the matching
 * `std::string` was stored under at insert time, regardless of which overload
 * the standard library implementation provides for `std::hash<std::string>`
 * (PRD §4.2 / §6.3 Route C).
 */
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
    /// Shard count — sized so the bottom 4 bits of a `std::hash<string_view>`
    /// pick a shard. A power-of-two count keeps the `& kShardMask` cheap and
    /// gives roughly even occupancy under any reasonable hash output.
    ///
    /// 16 shards is the recommendation in PRD §6.3 and is enough that a
    /// realistic ~30-key wide configuration distributes ~2 keys/shard, so an
    /// 8-thread `Find` storm under `LogTable::AppendBatch::RefreshColumnKeyIds`
    /// (PRD §4.8.2) hits ~uncontended shared-locks per shard.
    static constexpr size_t kShardCount = 16;
    static constexpr size_t kShardMask = kShardCount - 1;

    /// Per-shard fast map. Stored key type is `std::string` (the canonical
    /// owning copy) so heterogeneous `find(std::string_view)` works through
    /// the transparent hash + equality. KeyOf does **not** read this map —
    /// the canonical view it returns lives in `reverse` for pointer stability
    /// across rehashes.
    struct Shard
    {
        tsl::robin_map<std::string, KeyId, TransparentStringHash, TransparentStringEqual> map;
        mutable std::shared_mutex mutex;
    };
    std::array<Shard, kShardCount> shards;

    /// Reverse storage: KeyId index -> owning std::string. `std::deque` is
    /// used so that growing the reverse storage never invalidates pointers/
    /// views to previously inserted strings, which is required by `KeyOf`'s
    /// documented pointer-stability contract.
    std::deque<std::string> reverse;

    /// Atomic high-water mark of allocated KeyIds; equals `reverse.size()`
    /// once `reverseMutex` is unlocked. Stored separately so `Size()` can be
    /// read concurrently without taking any lock.
    std::atomic<size_t> size{0};

    /// Serialises `reverse.emplace_back` so the deque mutation observed by
    /// `KeyOf(id)` happens-before any other thread reads `id` via the per-
    /// shard map. Insert-only path; `KeyOf` and `Size()` do not take this
    /// lock. `SortedKeys()` snapshots under it.
    mutable std::mutex reverseMutex;

    /// Computes the shard index for @p key, using the same hash function the
    /// per-shard map uses internally. Result is in [0, kShardCount).
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

    // Fast path: heterogeneous `find(std::string_view)` under the per-shard
    // shared lock. Multiple readers across shards run wait-free; multiple
    // readers within the same shard share the lock without blocking.
    {
        std::shared_lock<std::shared_mutex> lock(shard.mutex);
        if (auto it = shard.map.find(key); it != shard.map.end())
        {
            return it->second;
        }
    }

    // Slow path: take both the shard's exclusive lock and the reverse-
    // storage lock atomically via std::scoped_lock's deadlock-avoiding
    // algorithm. Lock order is consistent across all call sites
    // (shard exclusive, then reverseMutex), but scoped_lock removes the need
    // to reason about it manually if a future caller adds more locks.
    std::scoped_lock locks(shard.mutex, mImpl->reverseMutex);

    // Double-check under the exclusive lock — another thread may have
    // inserted the key between our shared-lock find above and our acquiring
    // the exclusive lock.
    if (auto it = shard.map.find(key); it != shard.map.end())
    {
        return it->second;
    }

    // Append to the reverse deque first so the entry's address is stable
    // before any other thread observes the new id via the shard map.
    const KeyId id = static_cast<KeyId>(mImpl->reverse.size());
    mImpl->reverse.emplace_back(key);

    // Insert into the per-shard map. The stored std::string is a copy of the
    // canonical reverse-deque entry; we accept the duplication in exchange
    // for keeping the map's storage independent of the deque's pointer
    // stability contract. Heterogeneous lookup means callers never have to
    // materialise a std::string for a `find` query (PRD req. 4.2.1, 4.2.4).
    shard.map.emplace(mImpl->reverse.back(), id);

    // Publish the new size last, with release semantics, so a concurrent
    // `Size()` reader that observes the new value also observes the
    // underlying deque growth (synchronizes-with std::memory_order_acquire
    // in Size()).
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
    // The reverse deque only ever appends, and a successful KeyId implies the
    // entry at that index has been published (release-store on `size` in
    // GetOrInsert). std::deque guarantees pointer stability across appends, so
    // dereferencing without the reverseMutex is safe here.
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
