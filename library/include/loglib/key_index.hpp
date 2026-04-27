#pragma once

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#ifdef LOGLIB_KEY_INDEX_INSTRUMENTATION
#include <atomic>
#include <cstddef>
#endif

namespace loglib
{

/// Dense integer id for a log field key. Ids are assigned monotonically from 0
/// by `KeyIndex::GetOrInsert` and never reused, so they double as indices into
/// per-key arrays.
using KeyId = uint32_t;

/// Sentinel returned by `KeyIndex::Find` when a key is unknown.
inline constexpr KeyId kInvalidKeyId = std::numeric_limits<KeyId>::max();

/// Shared, lock-light, append-only dictionary mapping log field keys to dense
/// integer ids.
///
/// Thread-safety contract:
/// - `GetOrInsert` and `Find` are safe to call concurrently from multiple
///   threads with overlapping key sets.
/// - The `string_view` returned by `KeyOf` points into storage that is
///   pointer-stable for the lifetime of the `KeyIndex`; concurrent inserts do
///   not invalidate it.
/// - `Size` may grow between the call and the caller observing the value.
/// - `SortedKeys` snapshots under an internal lock; intended for cold paths.
class KeyIndex
{
public:
    KeyIndex();
    ~KeyIndex();

    KeyIndex(const KeyIndex &) = delete;
    KeyIndex &operator=(const KeyIndex &) = delete;

    KeyIndex(KeyIndex &&) noexcept;
    KeyIndex &operator=(KeyIndex &&) noexcept;

    /// Returns the canonical id for @p key, allocating a new one if the key has
    /// not been seen before. Thread-safe; concurrent calls share a single dense
    /// id space. The input view's contents are copied; it does not need to
    /// outlive the call.
    KeyId GetOrInsert(std::string_view key);

    /// Returns the canonical id for @p key, or `kInvalidKeyId` if it has not
    /// been inserted. Thread-safe.
    KeyId Find(std::string_view key) const;

    /// Returns the key string for @p id. The view is stable for the lifetime of
    /// the `KeyIndex`. Calling with an id outside `[0, Size())` is undefined.
    std::string_view KeyOf(KeyId id) const;

    /// Current number of registered keys (the high-water mark of allocated ids).
    size_t Size() const;

    /// Returns a copy of all keys, sorted lexicographically. Cold path; takes a
    /// snapshot under the internal lock.
    std::vector<std::string> SortedKeys() const;

#ifdef LOGLIB_KEY_INDEX_INSTRUMENTATION
    /// Test-only call counters for `GetOrInsert` and `Find`. Compiled in only
    /// when `LOGLIB_KEY_INDEX_INSTRUMENTATION` is defined, which the unit-test
    /// target enables and shipped builds do not.
    static std::atomic<std::size_t> sGetOrInsertCallCount;
    static std::atomic<std::size_t> sFindCallCount;

    static void ResetInstrumentationCounters() noexcept;
    static std::size_t LoadGetOrInsertCount() noexcept;
    static std::size_t LoadFindCount() noexcept;
#endif

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

} // namespace loglib
