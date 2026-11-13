/**
 * @brief Maps field names to dense integer ids.
 *
 * Every distinct key in a log, such as level or message, gets a KeyId the first time it is seen. Rows then store ids
 * instead of strings, and the ids index per-key arrays directly. One KeyIndex is shared by all rows in a LogData.
 *
 * This is part of the core module.
 */
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

/** @brief Small integer id for a field key. Ids start at 0, are never reused, and can index arrays kept per key. */
using KeyId = uint32_t;

inline constexpr KeyId INVALID_KEY_ID = std::numeric_limits<KeyId>::max();

/** @brief The largest key id a row can store: a field keeps its key in 24 bits. GetOrInsert throws beyond it. */
inline constexpr KeyId MAX_ROW_KEY_ID = 0xFFFFFFU;

/**
 * @brief Maps field keys to KeyIds. Keys are only ever added, and parser threads share one index.
 *
 * - GetOrInsert and Find may be called from several threads at once, including for the same key.
 * - A view returned by KeyOf stays valid for the life of the index, even while other threads insert.
 * - Size may already be out of date when the caller reads it, because other threads can add keys.
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

    /** @brief The id of key, assigned on first sight. The index copies the key's bytes. */
    [[nodiscard]] KeyId GetOrInsert(std::string_view key);

    /** @brief The id of key, or INVALID_KEY_ID if it was never inserted. */
    [[nodiscard]] KeyId Find(std::string_view key) const;

    /** @brief The key text for an id. The id must come from this index; anything else is undefined behavior. */
    [[nodiscard]] std::string_view KeyOf(KeyId id) const;

    [[nodiscard]] size_t Size() const noexcept;

    /** @brief All keys in sorted order. Takes the index's lock and builds a new list, so keep it off hot paths. */
    [[nodiscard]] std::vector<std::string> SortedKeys() const;

    /** @brief Approximate heap bytes used by the index. For the memory benchmark. */
    [[nodiscard]] size_t EstimatedMemoryBytes() const;

#ifdef LOGLIB_KEY_INDEX_INSTRUMENTATION
    /** @brief Test-only call counters compiled in by the unit-test target. */
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
