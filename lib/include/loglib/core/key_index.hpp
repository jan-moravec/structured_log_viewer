/**
 * @brief Maps field names to dense integer ids.
 *
 * Every distinct key in a log, such as level or message, gets a KeyId the first time it is seen. Rows then store ids
 * instead of strings, and the ids index per-key arrays directly. One KeyIndex is shared by all rows in a LogData.
 *
 * This is part of the core module.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace loglib
{

namespace internal
{
struct MemoryStats;
} // namespace internal

/** @brief Small integer id for a field key. Ids start at 0, are never reused, and can index arrays kept per key. */
using KeyId = uint32_t;

inline constexpr KeyId INVALID_KEY_ID = std::numeric_limits<KeyId>::max();

/**
 * @brief The largest key id a row can store. A field keeps its key in 24 bits, and GetOrInsert throws LimitError past
 * this id.
 */
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

private:
    friend struct internal::MemoryStats;

    /** @brief Approximate heap bytes used by the index. For the memory benchmark. */
    [[nodiscard]] size_t EstimatedMemoryBytes() const;

    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

} // namespace loglib
