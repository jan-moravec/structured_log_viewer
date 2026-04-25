#include "loglib/key_index.hpp"

#include <oneapi/tbb/concurrent_hash_map.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace loglib
{

namespace
{

/**
 * @brief HashCompare for `tbb::concurrent_hash_map<std::string, KeyId>`.
 *
 * `tbb::concurrent_hash_map` does not support heterogeneous lookup the way
 * `std::unordered_map` does (no `is_transparent`), so callers that hold a
 * `std::string_view` must materialise a `std::string` for the lookup. The hash
 * function still goes through `std::hash<std::string_view>` so the produced
 * bucket index would match a hypothetical heterogeneous lookup if TBB ever
 * grows one.
 *
 * The fast path for in-pipeline lookups is the per-worker key cache (a
 * transparently-hashable `tsl::robin_map<std::string, KeyId>`), which is hit
 * before the canonical `KeyIndex` is consulted; only key-cache misses pay the
 * `std::string` materialisation cost here.
 */
struct StringHashCompare
{
    static size_t hash(const std::string &key) noexcept
    {
        return std::hash<std::string_view>{}(key);
    }
    static bool equal(const std::string &lhs, const std::string &rhs) noexcept
    {
        return lhs == rhs;
    }
};

} // namespace

struct KeyIndex::Impl
{
    /// Forward map: key string -> KeyId. tbb::concurrent_hash_map provides
    /// fine-grained locking and lock-free reads on common code paths.
    using ForwardMap = oneapi::tbb::concurrent_hash_map<std::string, KeyId, StringHashCompare>;
    ForwardMap forward;

    /// Reverse storage: KeyId index -> owning std::string. std::deque is used
    /// so that growing the reverse storage never invalidates pointers/views to
    /// previously inserted strings, which is required by KeyOf's documented
    /// pointer-stability contract.
    std::deque<std::string> reverse;

    /// Atomic high-water mark of allocated KeyIds; equals `reverse.size()` once
    /// `reverseMutex` is unlocked. Stored separately so `Size()` can be read
    /// concurrently without taking the reverse-storage mutex.
    std::atomic<size_t> size{0};

    /// Serialises `reverse.emplace_back` so the deque mutation observed by
    /// `KeyOf(id)` happens-before any other thread reads `id` via the forward
    /// map. Insert-only path; `KeyOf` does not take this lock.
    mutable std::mutex reverseMutex;
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
    // Materialise a std::string for the TBB lookup. concurrent_hash_map
    // doesn't support heterogeneous lookup, but per-worker caches absorb this
    // cost in the parsing hot path, so this slower call site is only exercised
    // on cache misses (and from the per-batch dedup path in Stage B).
    const std::string keyOwned(key);

    // Fast path: the key already exists. concurrent_hash_map::find acquires a
    // shared lock on the matching bucket, so concurrent fast-path lookups do
    // not block each other.
    {
        Impl::ForwardMap::const_accessor acc;
        if (mImpl->forward.find(acc, keyOwned))
        {
            return acc->second;
        }
    }

    // Slow path: the key may need to be inserted. Take the reverse-storage
    // mutex *before* the concurrent_hash_map::insert so two racing inserts of
    // the same key serialize on this lock and we do not allocate two
    // consecutive KeyIds for the same string.
    std::scoped_lock lock(mImpl->reverseMutex);

    // Double-check under the lock — another thread may have inserted the key
    // between our find above and our acquiring the mutex.
    {
        Impl::ForwardMap::const_accessor acc;
        if (mImpl->forward.find(acc, keyOwned))
        {
            return acc->second;
        }
    }

    // Append to the reverse deque first so the entry's address is stable
    // before any other thread observes the new id via the forward map.
    const KeyId id = static_cast<KeyId>(mImpl->reverse.size());
    mImpl->reverse.emplace_back(keyOwned);

    Impl::ForwardMap::accessor acc;
    [[maybe_unused]] const bool inserted = mImpl->forward.insert(acc, mImpl->reverse.back());
    acc->second = id;

    // Publish the new size last, with release semantics, so a concurrent
    // `Size()` reader that observes the new value also observes the underlying
    // deque growth (synchronizes-with std::memory_order_acquire in Size()).
    mImpl->size.store(mImpl->reverse.size(), std::memory_order_release);
    return id;
}

KeyId KeyIndex::Find(std::string_view key) const
{
#ifdef LOGLIB_KEY_INDEX_INSTRUMENTATION
    sFindCallCount.fetch_add(1, std::memory_order_relaxed);
#endif
    const std::string keyOwned(key);
    Impl::ForwardMap::const_accessor acc;
    if (mImpl->forward.find(acc, keyOwned))
    {
        return acc->second;
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
