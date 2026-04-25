#pragma once

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace loglib
{

/**
 * @brief Dense integer identifier for a log field key.
 *
 * KeyIds are assigned monotonically starting from 0 by `KeyIndex::GetOrInsert`
 * and never reused. Code that consumes a KeyId can therefore use it as an index
 * into per-key arrays (e.g. the per-worker parse cache) without an extra
 * indirection table.
 */
using KeyId = uint32_t;

/**
 * @brief Sentinel value returned by `KeyIndex::Find` when a key is unknown.
 *
 * Distinct from any value returned by `KeyIndex::GetOrInsert`, which always
 * returns an in-range id. Equal to `std::numeric_limits<KeyId>::max()`.
 */
inline constexpr KeyId kInvalidKeyId = std::numeric_limits<KeyId>::max();

/**
 * @brief Shared, lock-light, append-only dictionary mapping log field keys to
 *        dense integer ids.
 *
 * `KeyIndex` is the single source of truth for the set of keys observed in a
 * `LogData`. It exists so that `LogLine`s can store a `KeyId` per field rather
 * than copying the field name as a `std::string`, and so that the parsing
 * pipeline can look up the canonical id of a key from any worker thread without
 * synchronising on a global mutex.
 *
 * ### Thread-safety contract (PRD req. 4.1.2 / 4.1.2a)
 *
 * - `GetOrInsert(std::string_view)` is safe to call concurrently from multiple
 *   threads, including with overlapping key sets. It returns the canonical
 *   `KeyId` for `key`, allocating a new id (the next consecutive value) if the
 *   key was not previously known.
 * - `Find(std::string_view) const` is safe to call concurrently with
 *   `GetOrInsert`. It returns `kInvalidKeyId` if the key has not been inserted.
 * - `KeyOf(KeyId) const` returns a `std::string_view` that points into storage
 *   that lives for the entire lifetime of the `KeyIndex` (pointer-stable across
 *   concurrent inserts). The view is invalidated only when the `KeyIndex` is
 *   destroyed or moved-from.
 * - `Size() const` returns the current high-water mark of allocated ids; it is
 *   safe to call concurrently with `GetOrInsert`. The result may grow between
 *   the call and the caller observing the value.
 * - `SortedKeys() const` is intended for cold paths (e.g. building the legacy
 *   `std::vector<std::string>` view exposed by `LogData::Keys()`). It takes a
 *   snapshot under the same internal lock used by inserts, which is acceptable
 *   for the diagnostic / configuration-UI use cases that call it.
 *
 * Pipeline Stage C (the `serial_in_order` appender) is the only stage that
 * needs the "high-water mark slice" trick described in PRD req. 4.1.2/2a:
 * snapshot `prevSize = Size()` once at start, then after each batch read
 * `currentSize = Size()` and emit `KeyOf` for every id in
 * `[prevSize, currentSize)` as the batch's set of newly-introduced keys. Since
 * ids are dense and never reordered, this slice is exact.
 */
class KeyIndex
{
public:
    KeyIndex();
    ~KeyIndex();

    KeyIndex(const KeyIndex &) = delete;
    KeyIndex &operator=(const KeyIndex &) = delete;

    KeyIndex(KeyIndex &&) noexcept;
    KeyIndex &operator=(KeyIndex &&) noexcept;

    /**
     * @brief Returns the canonical id for @p key, allocating a new one if the
     *        key has not been seen before.
     *
     * Thread-safe. Concurrent calls with overlapping or distinct keys both
     * yield a single dense id space (every key maps to exactly one id, ids are
     * consecutive starting at 0).
     *
     * @param key The key string to canonicalise. The contents are copied into
     *            internal storage; the input view does not need to outlive the
     *            call.
     * @return The canonical id, in range `[0, Size())`.
     */
    KeyId GetOrInsert(std::string_view key);

    /**
     * @brief Returns the canonical id for @p key without inserting.
     *
     * Thread-safe. Returns `kInvalidKeyId` if @p key has not been inserted.
     */
    KeyId Find(std::string_view key) const;

    /**
     * @brief Returns the key string for the given id.
     *
     * The returned view is stable for the lifetime of the `KeyIndex`. Calling
     * with an id outside `[0, Size())` is undefined behaviour.
     */
    std::string_view KeyOf(KeyId id) const;

    /**
     * @brief Returns the current number of registered keys (i.e. the high-water
     *        mark of allocated ids).
     */
    size_t Size() const;

    /**
     * @brief Returns a copy of all keys, sorted lexicographically.
     *
     * Cold path: takes a snapshot under the internal lock and is intended for
     * diagnostics, configuration UI, and the legacy
     * `LogData::Keys() -> std::vector<std::string>` accessor. Hot paths should
     * use `KeyOf` over the high-water-mark slice instead.
     */
    std::vector<std::string> SortedKeys() const;

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

} // namespace loglib
