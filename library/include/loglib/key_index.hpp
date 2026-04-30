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

/// Dense integer id for a log field key. Ids are assigned monotonically from
/// 0 and never reused, so they double as indices into per-key arrays.
using KeyId = uint32_t;

inline constexpr KeyId kInvalidKeyId = std::numeric_limits<KeyId>::max();

/// Shared, lock-light, append-only dictionary mapping keys to dense ids.
///
/// Thread-safety contract:
/// - `GetOrInsert` and `Find` are safe to call concurrently with overlapping
///   key sets.
/// - The `string_view` returned by `KeyOf` is pointer-stable for the lifetime
///   of the `KeyIndex`; concurrent inserts do not invalidate it.
/// - `Size` may grow between the call and the caller observing the value.
class KeyIndex
{
public:
    KeyIndex();
    ~KeyIndex();

    KeyIndex(const KeyIndex &) = delete;
    KeyIndex &operator=(const KeyIndex &) = delete;

    KeyIndex(KeyIndex &&) noexcept;
    KeyIndex &operator=(KeyIndex &&) noexcept;

    /// Returns the canonical id for @p key, allocating one if first-seen.
    /// Thread-safe; the input view's bytes are copied internally.
    [[nodiscard]] KeyId GetOrInsert(std::string_view key);

    /// Returns `kInvalidKeyId` if @p key has not been inserted. Thread-safe.
    [[nodiscard]] KeyId Find(std::string_view key) const;

    /// Stable for the lifetime of the `KeyIndex`. UB if @p id is out of range.
    [[nodiscard]] std::string_view KeyOf(KeyId id) const;

    [[nodiscard]] size_t Size() const noexcept;

    /// Cold-path snapshot under the internal lock.
    [[nodiscard]] std::vector<std::string> SortedKeys() const;

    /// Approximate heap bytes owned by the index (shard maps + reverse
    /// table). Used by the memory-footprint benchmark; not part of the
    /// parse hot path.
    [[nodiscard]] size_t EstimatedMemoryBytes() const;

#ifdef LOGLIB_KEY_INDEX_INSTRUMENTATION
    /// Test-only call counters compiled in by the unit-test target.
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
